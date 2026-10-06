"""Cases for test_live_session; collected by its legacy facade."""

import struct
import zlib
import math

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


class LiveSessionEffectCases:
    def test_config_free_replay_laser_depth_uses_tactical_clips(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-config-free-laser-depth-"))
        samples = {}
        # Same fixed drawn pose as the review repro; only the bridge differs.
        for name, camera in (("no-bridge", None), ("with-bridge", DUEL_CAMERA)):
            code, result = self._run(directory, name, (*DUEL_ARGS,
                "--eawr-live-step", "1", "--eawr-live-ticks", "180", "--eawr-live-capture-ticks", "90",
                "--eawr-space-camera", "0,707.10678,2207.10678,0,0,1500,0,1,0,60,10,7000",
                "--eawr-shadows", "on"), session=("--eawr-live-session", "replay"), camera=camera)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            self.assertEqual(result["live_session"]["rejected"], [])
            render = result["capture_identity"]["camera"]
            self.assertEqual((render["near"], render["far"]), (10, 60000))
            depth = result["battle_effects"]["laser_depth"]
            self.assertEqual(depth["clips"], [10, 7000])
            for render_name, scale in (("beam", 8), ("kite", 1.2)):
                sample = depth[f"first_{render_name}"]
                self.assertIsNotNone(sample)
                expected = (sample["view_depth"] - 10) / (7000 - 10)
                self.assertAlmostEqual(sample["normalized_depth"], expected, delta=0.00001)
                self.assertAlmostEqual(sample["factor"], 1 + scale * expected, delta=0.00002)
            samples[name] = depth
        for render_name in ("beam", "kite"):
            for field in ("view_depth", "factor", "half_width"):
                self.assertAlmostEqual(samples["no-bridge"][f"first_{render_name}"][field],
                                       samples["with-bridge"][f"first_{render_name}"][field], delta=0.00002)

    def test_laser_depth_uses_tactical_clips_at_three_distances(self):
        from tools.inventory.corpus import Corpus
        import xml.etree.ElementTree as ET
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-tactical-laser-depth-"))
        corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
        source = corpus.read_effective("foc", "data/xml/tacticalcameras.xml")
        self.assertIsNotNone(source)
        xml = directory / "legacy/Data/XML"
        xml.mkdir(parents=True)
        (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
        root = ET.fromstring(source.data)
        mode = next(node for node in root if node.get("Name", "").lower() == "space_mode")
        mode.find("Far_Clip").text = "60000"
        ET.ElementTree(root).write(xml / "TacticalCameras.xml", encoding="utf-8")
        shutil.copyfile(DUEL_CAMERA.parent / "space-live-camera-bindings.json", directory / "space-live-camera-bindings.json")
        measurements = []
        for distance in (1000, 1500, 1900):
            camera = directory / f"camera-{distance}.xml"
            root = ET.fromstring(DUEL_CAMERA.read_text(encoding="utf-8"))
            root.find("initial").set("zoom", str((distance - 200) / 1700))
            root.remove(root.find("constant_overrides"))
            ET.ElementTree(root).write(camera, encoding="utf-8")
            samples = {}
            for name, far, mod in (("legacy", 60000, ("--eawr-mod-root", str(xml.parent.parent))),
                                    ("tactical", 7000, ())):
                code, result = self._run(directory, f"{name}_{distance}", (*DUEL_ARGS, *mod,
                    "--eawr-live-step", "1", "--eawr-live-ticks", "180", "--eawr-live-capture-ticks", "90",
                    "--eawr-shadows", "on"), session=("--eawr-live-session", "replay"), camera=camera)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                self.assertEqual(result["live_session"]["rejected"], [])
                render = result["capture_identity"]["camera"]
                self.assertEqual((render["near"], render["far"]), (10, 60000))
                self.assertAlmostEqual(math.dist(render["position"], render["target"]), distance, delta=0.05)
                depth = result["battle_effects"]["laser_depth"]
                self.assertEqual(depth["clips"], [10, far])
                for render_name, scale in (("beam", 8), ("kite", 1.2)):
                    sample = depth[f"first_{render_name}"]
                    self.assertIsNotNone(sample)
                    expected = (sample["view_depth"] - 10) / (far - 10)
                    self.assertAlmostEqual(sample["normalized_depth"], expected, delta=0.00001)
                    self.assertAlmostEqual(sample["factor"], 1 + scale * expected, delta=0.00002)
                samples[name] = depth
            for render_name in ("beam", "kite"):
                self.assertAlmostEqual(samples["legacy"][f"first_{render_name}"]["view_depth"],
                                       samples["tactical"][f"first_{render_name}"]["view_depth"], delta=0.02)
                self.assertGreater(samples["tactical"][f"first_{render_name}"]["factor"],
                                   samples["legacy"][f"first_{render_name}"]["factor"])
            measurements.append({"distance": distance, **samples})
        (directory / "laser-depth-measurements.json").write_text(json.dumps(measurements, indent=2), encoding="utf-8")

    def test_static_bomb_root_particle_and_countdown_audio(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-static-bomb-"))
        fleet = ("--eawr-skirmish-slot", "1:Empire:0:human",
                 "--eawr-skirmish-slot", "2:Rebel:1:human",
                 "--eawr-skirmish-fleet", "1:Slave_I", "--eawr-skirmish-fleet", "2:none",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-player", "1")
        code, roster = self._run(directory, "roster", (*fleet, "--eawr-live-ticks", "8"),
                                  session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, roster.get("failure"))
        source = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == "Slave_I")
        for variant, step in (("paced", 1), ("catch-up", 3), ("death-precedence", 1)):
            with self.subTest(variant=variant):
                mod_args = ()
                if variant == "death-precedence":
                    import xml.etree.ElementTree as ET
                    from tools.inventory.corpus import Corpus
                    xml = directory / "mod/Data/XML"
                    xml.mkdir(parents=True)
                    (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                    authored = Corpus(os.environ["EAWR_EAW_GAME_ROOT"]).read_effective("foc", "data/xml/projectiles.xml")
                    tree = ET.fromstring(authored.data)
                    bomb = next(node for node in tree if node.get("Name") == "Proj_Harmonic_Bomb_Slave_I")
                    bomb.find("Projectile_SFXEvent_Detonate").text = "SFX_Proton_Torpedo_Detonation"
                    bomb.find("Projectile_Lifetime_Detonation_Particle").text = "Large_Explosion_Space"
                    ET.ElementTree(tree).write(xml / "projectiles.xml", encoding="utf-8")
                    mod_args = ("--eawr-mod-root", str(xml.parent.parent))
                code, result = self._run(directory, f"bomb-{variant}", (*fleet,
                    "--eawr-live-step", str(step), "--eawr-live-ticks", "110",
                    "--eawr-live-order", f"30:ability:{source}@HARMONIC_BOMB,on",
                    "--eawr-live-follow", str(source), "--eawr-live-audio-pace", "on",
                    "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                    "--eawr-live-capture-ticks", "24,42,60,72,84", *mod_args),
                    session=("--eawr-live-session", "skirmish"), camera=None)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertEqual(result["live_session"]["rejected"], [])
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                effects = result["battle_effects"]
                self.assertEqual(effects["projectile_models"]["Proj_Harmonic_Bomb_Slave_I"]["max_bound"], 1)
                self.assertNotIn("Proj_Harmonic_Bomb_Slave_I", effects["projectiles_not_drawn"], effects)
                rows = [row for row in result["unit_emitters"]["start_log"] if row["proxy"] == "p_seismic_bomb00.ALO"]
                self.assertEqual(len(rows), 1, rows)
                self.assertIsNotNone(rows[0]["first_age"], rows)
                self.assertGreater(rows[0]["max_visible_quads"], 0, rows)
                self.assertEqual(effects["spawned"].get("hero_detonation:Harmonic_Bomb_Explosion_Slave_I"), 1, effects)
                requested = result["battle_audio"]["requested"]
                self.assertEqual(requested.get("projectile_detonation:SFX_Sizemic_Detonation"), 1, requested)
                self.assertNotIn("projectile_detonation:SFX_Proton_Torpedo_Detonation", requested)
                self.assertNotIn("hero_detonation:Large_Explosion_Space", effects["spawned"])

    def test_home_one_secondary_defend_and_purchase_slots_compose_shells(self):
        # BP-22: a hero's second ability has the same shell as a primary DEFEND.
        # Arrival slots must be prepared too, while placement previews stay unshielded.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-home-one-defend-"))
        fleet = ("--eawr-skirmish-slot", "1:Rebel:0:human",
                 "--eawr-skirmish-slot", "2:Empire:1:human",
                 "--eawr-skirmish-fleet", "1:none",
                 "--eawr-skirmish-fleet", "2:none",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                 "--eawr-live-player", "1", "--eawr-live-step", "1",
                 "--eawr-live-purchase-slots", "1",
                 "--eawr-environment", "map", "--eawr-lighting", "sh",
                 "--eawr-shadows", "on")
        code, reserve = self._run(directory, "reserve", (*fleet, "--eawr-live-ticks", "8"),
                                  session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, reserve.get("failure"))
        self.assertFalse(any(row["type"] == "Home_One" for row in reserve["live_session"]["start_fleet"]))
        shells = reserve["populate"]["live_units"]["shield_shells"]
        self.assertEqual(shells["types"].get("Home_One"), "composed", shells)
        for ordinary in ("Nebulon_B_Frigate", "Calamari_Cruiser"):
            self.assertEqual(shells["types"].get(ordinary), "composed", shells)
        self.assertEqual(shells["max_shown"], 0, shells)

        hero_fleet = (*fleet, "--eawr-skirmish-fleet", "1:Home_One")
        code, roster = self._run(directory, "roster", (*hero_fleet, "--eawr-live-ticks", "8"),
                                 session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, roster.get("failure"))
        hero = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == "Home_One")
        frames = {}
        for name, active in (("before", False), ("after", True)):
            orders = ("--eawr-live-order", f"20:ability:{hero}@DEFEND,on") if active else ()
            code, result = self._run(directory, name, (*hero_fleet, *orders,
                "--eawr-live-follow", str(hero), "--eawr-live-ticks", "45",
                "--eawr-live-capture-ticks", "30"), session=("--eawr-live-session", "skirmish"), camera=None)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertEqual(result["live_session"]["rejected"], [])
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            shells = result["populate"]["live_units"]["shield_shells"]
            self.assertEqual(shells["types"].get("Home_One"), "composed", shells)
            self.assertEqual(shells["max_shown"], int(active), shells)
            frames[name] = decode_png((directory / f"{name}_t0030.png").read_bytes())
        self.assertNotEqual(frames["before"][2], frames["after"][2])

    def test_concentrate_fire_button_target_click_reaches_simulation(self):
        directory = ROOT / "out/godot/hero-targets-ui"
        directory.mkdir(parents=True, exist_ok=True)
        seed = directory / "seed.eawr-replay"
        fleet = ("--eawr-skirmish-slot", "1:Rebel:0:human",
                 "--eawr-skirmish-slot", "2:Rebel:1:human",
                 "--eawr-skirmish-fleet", "1:Home_One,Corellian_Corvette",
                 "--eawr-skirmish-fleet", "2:Nebulon_B_Frigate",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                 "--eawr-live-player", "1", "--eawr-live-step", "1",
                 "--eawr-live-ticks", "8", "--eawr-live-replay-out", str(seed))
        code, idle = self._run(directory, "roster", fleet, session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, idle.get("failure"))
        source = next(row["entity"] for row in idle["live_session"]["start_fleet"] if row["type"] == "Home_One")
        target = next(row["entity"] for row in idle["live_session"]["start_fleet"] if row["type"] == "Nebulon_B_Frigate")
        recruit = next(row["entity"] for row in idle["live_session"]["start_fleet"] if row["type"] == "Corellian_Corvette")
        data = bytearray(seed.read_bytes())
        self.assertEqual(data[:8], b"EAWRPLY\0")
        self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 0)
        header = struct.unpack_from("<H", data, 10)[0]
        players = struct.unpack_from("<I", data, 48)[0]
        count = struct.unpack_from("<Q", data, 56)[0]
        found = set()
        for index in range(count):
            offset = header + players * 24 + index * 80
            entity = struct.unpack_from("<Q", data, offset)[0]
            if entity not in (source, target, recruit): continue
            found.add(entity)
            x, y = (-800, -1500) if entity == source else (-500, -2000) if entity == recruit else (1000, -1500)
            struct.pack_into("<7q", data, offset + 24, x << 24, y << 24, 0, 0, 0, 0, 1 << 24)
        self.assertEqual(found, {source, target, recruit})
        struct.pack_into("<Q", data, 40, 400)
        replay = directory / "target-click.eawr-replay"
        replay.write_bytes(data)
        args = ("--eawr-live-replay", str(replay), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "350",
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
        camera = ROOT / "apps/viewer/project/config/coruscant-hero-targets-camera.xml"
        code, control = self._run(directory, "control", args, session=("--eawr-live-session", "replay"), camera=camera)
        self.assertEqual(code, 0, control.get("failure"))
        self.assertFalse(any(row["shooter"] == recruit and row["target"] == target
                             for row in control["live_session"]["first_hits"]), control["live_session"]["first_hits"])
        clicked = (*args,
                "--eawr-live-input", f"20:click:unit={source}",
                "--eawr-live-input", "25:click:ability=0",
                "--eawr-live-input", f"30:click:unit={target}",
                "--eawr-live-capture-ticks", "24,29,35,55,300")
        code, result = self._run(directory, "clicked", clicked, session=("--eawr-live-session", "replay"), camera=camera)
        self.assertEqual(code, 0, result.get("failure"))
        live, battle = result["live_session"], result["battle_input"]
        self.assertEqual(live["ability_requests"], {"issued": 1, "refused": 0})
        self.assertEqual(live["rejected"], [])
        self.assertTrue(live["headless_hashes_equal"])
        self.assertEqual(battle["ability_bar"]["targeted"], 1, battle["log"])
        self.assertIn(f"ability CONCENTRATE_FIRE at {target}", battle["log"])
        self.assertTrue(any(row["shooter"] == recruit and row["target"] == target and row["tick"] > 30
                            for row in live["first_hits"]), live["first_hits"])

    def test_vader_replenishes_one_missing_escort_through_real_button(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hero-g9-"))
        seed = directory / "seed.eawr-replay"
        fleet = ("--eawr-skirmish-slot", "1:Empire:0:human",
                 "--eawr-skirmish-slot", "2:Rebel:1:human",
                 "--eawr-skirmish-fleet", "1:Darth_Vader_TIE_Fighter_Squadron",
                 "--eawr-skirmish-fleet", "2:Corellian_Corvette",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                 "--eawr-live-player", "1", "--eawr-live-step", "1",
                 "--eawr-live-ticks", "8", "--eawr-live-replay-out", str(seed))
        code, roster = self._run(directory, "roster", fleet, session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, roster.get("failure"))
        replay = bytearray(seed.read_bytes())
        self.assertEqual(replay[:8], b"EAWRPLY\0")
        self.assertEqual(struct.unpack_from("<Q", replay, 64)[0], 0)
        header = struct.unpack_from("<H", replay, 10)[0]
        players = struct.unpack_from("<I", replay, 48)[0]
        count = struct.unpack_from("<Q", replay, 56)[0]
        # start_fleet lists companies, not craft. Read the real authored seed;
        # object type IDs are CRC32 of uppercase names (assets::object_type_crc).
        records = [struct.unpack_from("<QQI", replay, header + players * 24 + index * 80)
                   for index in range(count)]
        leader_type = zlib.crc32(b"TIE_PROTOTYPE")
        escort_type = zlib.crc32(b"ESCORT_TIE_FIGHTER")
        leaders = [entity for entity, kind, owner in records if owner == 1 and kind == leader_type]
        escorts = [entity for entity, kind, owner in records if owner == 1 and kind == escort_type]
        self.assertEqual(len(leaders), 1, records)
        self.assertEqual(len(escorts), 6, records)
        source = leaders[0]
        parent = next(row["entity"] for row in roster["live_session"]["start_fleet"]
                      if row["type"] == "Darth_Vader_TIE_Fighter_Squadron")
        framed = {parent, source, *escorts}
        for index in range(count):
            offset = header + players * 24 + index * 80
            entity = struct.unpack_from("<Q", replay, offset)[0]
            owner = struct.unpack_from("<I", replay, offset + 16)[0]
            # Keep stations/default companies away from the fighter pick point.
            x, y = (-400, -1500) if entity in framed else ((-5000 if owner == 1 else 5000), -5000)
            struct.pack_into("<7q", replay, offset + 24,
                             x << 24, y << 24, 0,
                             0, 0, 0, 1 << 24)
        struct.pack_into("<Q", replay, 40, 180)
        path = directory / "vader.eawr-replay"
        path.write_bytes(replay)
        code, result = self._run(directory, "wingmen", (
            "--eawr-live-replay", str(path), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
            "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "90",
            "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            "--eawr-live-order", f"15:damage:{escorts[0]}@1000000",
            "--eawr-live-input", f"20:click:unit={source}",
            "--eawr-live-input", "30:click:ability=0",
            "--eawr-live-capture-ticks", "14,25,35,50,80"),
            session=("--eawr-live-session", "replay"),
            camera=ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml")
        self.assertEqual(code, 0, result.get("failure"))
        self.assertEqual(result["live_session"]["ability_requests"], {"issued": 1, "refused": 0})
        self.assertEqual(result["live_session"]["rejected"], [])
        self.assertTrue(result["live_session"]["headless_hashes_equal"])
        self.assertEqual([button["ability"] for button in result["battle_input"]["ability_bar"]["buttons"]],
                         ["REPLENISH_WINGMEN"])
        restored = next(group["members"] for group in result["live_session"]["squadrons"]
                        if group["container"] == parent)
        self.assertEqual(len(restored), 7)
        self.assertEqual(restored[:-1], [source, *escorts[1:]])
        self.assertNotIn(escorts[0], restored)
        self.assertNotIn(restored[-1], framed)
        self.assertTrue(any(name.startswith("hero_wingmen:") and count >= 7
                            for name, count in result["battle_effects"]["spawned"].items()), result["battle_effects"])

    def test_hero_bomb_and_weaken_buttons_spawn_authored_effects(self):
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hero-g8-"))
        directory.mkdir(parents=True, exist_ok=True)
        camera = ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml"
        for name, hero, faction, target_type, enemy_faction in (
            ("bomb", "Slave_I", "Empire", "Corellian_Corvette", "Rebel"),
            ("weaken", "Sundered_Heart", "Rebel", "Tartan_Patrol_Cruiser", "Empire"),
        ):
            with self.subTest(ability=name):
                seed = directory / f"{name}-seed.eawr-replay"
                fleet = ("--eawr-skirmish-slot", f"1:{faction}:0:human",
                         "--eawr-skirmish-slot", f"2:{enemy_faction}:1:human",
                         "--eawr-skirmish-fleet", f"1:{hero}",
                         "--eawr-skirmish-fleet", f"2:{target_type}",
                         "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                         "--eawr-live-player", "1", "--eawr-live-step", "1",
                         "--eawr-live-ticks", "8", "--eawr-live-replay-out", str(seed))
                code, roster = self._run(directory, f"{name}-roster", fleet,
                                         session=("--eawr-live-session", "skirmish"))
                self.assertEqual(code, 0, roster.get("failure"))
                source = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == hero)
                target = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == target_type)
                replay = bytearray(seed.read_bytes())
                self.assertEqual(replay[:8], b"EAWRPLY\0")
                self.assertEqual(struct.unpack_from("<Q", replay, 64)[0], 0)
                header = struct.unpack_from("<H", replay, 10)[0]
                players = struct.unpack_from("<I", replay, 48)[0]
                count = struct.unpack_from("<Q", replay, 56)[0]
                staged = set()
                for index in range(count):
                    offset = header + players * 24 + index * 80
                    entity = struct.unpack_from("<Q", replay, offset)[0]
                    if entity not in (source, target): continue
                    staged.add(entity)
                    struct.pack_into("<7q", replay, offset + 24,
                                     (-250 if entity == source else 250) << 24, -1500 << 24, 0,
                                     0, 0, 0, 1 << 24)
                self.assertEqual(staged, {source, target})
                struct.pack_into("<Q", replay, 40, 180)
                path = directory / f"{name}.eawr-replay"
                path.write_bytes(replay)
                aim = ("--eawr-live-input", "35:click:@250,-1500,0") if name == "weaken" else ()
                code, result = self._run(directory, name, (
                    "--eawr-live-replay", str(path), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                    "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "150",
                    "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                    "--eawr-live-capture-ticks", "29,40,74,80,110",
                    "--eawr-live-input", f"20:click:unit={source}",
                    "--eawr-live-input", "30:click:ability=0", *aim), session=("--eawr-live-session", "replay"), camera=camera)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertEqual(result["live_session"]["rejected"], [])
                self.assertEqual(result["live_session"]["ability_requests"], {"issued": 1, "refused": 0})
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                effects = result["battle_effects"]
                self.assertIsNone(effects["failure"])
                self.assertEqual(sum(count for key, count in effects["spawned"].items()
                                     if key.startswith("hero_detonation:")), 1, effects)
                self.assertFalse(any(key.startswith("lifetime_detonation:")
                                     for key in effects["spawned"]), effects)
                if name == "bomb":
                    self.assertGreater(effects["projectile_models"]["Proj_Harmonic_Bomb_Slave_I"]["max_bound"], 0, effects)
                    self.assertNotIn("Proj_Harmonic_Bomb_Slave_I", effects["projectiles_not_drawn"], effects)
                    emitters = result["unit_emitters"]
                    self.assertEqual(emitters["started"].get("p_seismic_bomb00.ALO"), 1, emitters)
                    bomb_rows = [row for row in emitters["start_log"] if row["proxy"] == "p_seismic_bomb00.ALO"]
                    self.assertEqual(len(bomb_rows), 1, bomb_rows)
                    self.assertIsNotNone(bomb_rows[0]["first_age"], bomb_rows)
                    self.assertGreater(bomb_rows[0]["max_visible_quads"], 0, bomb_rows)
                    self.assertEqual(effects["spawned"].get("hero_detonation:Harmonic_Bomb_Explosion_Slave_I"), 1, effects)
                    requested = result["battle_audio"]["requested"]
                    self.assertEqual(requested.get("projectile_detonation:SFX_Sizemic_Detonation"), 1, requested)
                else:
                    self.assertEqual(effects["spawned"].get("hero_detonation:Weaken_Enemy_Detonation_Effect"), 1, effects)
                    self.assertEqual(result["battle_input"]["ability_bar"]["targeted"], 1)
                    self.assertGreater(effects["spawned"].get("weaken_status:Weaken_Enemy_Particle_Effect", 0), 0, effects)

    def test_hero_energy_and_tractor_beams_are_live_and_release(self):
        # Build a fresh recorded setup through the real roster gate, then place its two ships
        # in range by editing only the replay's documented fixed-width setup coordinates.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-hero-g7-"))
        directory.mkdir(parents=True, exist_ok=True)
        seed_replay = directory / "seed.eawr-replay"
        fleet = ("--eawr-skirmish-slot", "1:Empire:0:human",
                 "--eawr-skirmish-slot", "2:Rebel:1:human",
                 "--eawr-skirmish-fleet", "1:Accuser_Star_Destroyer",
                 "--eawr-skirmish-fleet", "2:Nebulon_B_Frigate",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                 "--eawr-live-player", "1", "--eawr-live-step", "1",
                 "--eawr-live-ticks", "8", "--eawr-live-replay-out", str(seed_replay))
        code, idle = self._run(directory, "roster", fleet, session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, idle.get("failure"))
        source = next(row["entity"] for row in idle["live_session"]["start_fleet"] if row["type"] == "Accuser_Star_Destroyer")
        target = next(row["entity"] for row in idle["live_session"]["start_fleet"] if row["type"] == "Nebulon_B_Frigate")
        replay = bytearray(seed_replay.read_bytes())
        self.assertEqual(replay[:8], b"EAWRPLY\0")
        self.assertEqual(struct.unpack_from("<Q", replay, 64)[0], 0, "AI-off seed has no recorded commands")
        header = struct.unpack_from("<H", replay, 10)[0]
        players = struct.unpack_from("<I", replay, 48)[0]
        count = struct.unpack_from("<Q", replay, 56)[0]
        found = set()
        for index in range(count):
            offset = header + players * 24 + index * 80
            entity = struct.unpack_from("<Q", replay, offset)[0]
            if entity not in (source, target): continue
            found.add(entity)
            struct.pack_into("<7q", replay, offset + 24,
                             (-400 if entity == source else 400) << 24, -1500 << 24, 0,
                             0, 0, 0, 1 << 24)
        self.assertEqual(found, {source, target})
        struct.pack_into("<Q", replay, 40, 160)
        staged = directory / "beams.eawr-replay"
        staged.write_bytes(replay)
        camera = ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml"
        common = ("--eawr-live-replay", str(staged), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                  "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                  "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "140",
                  "--eawr-live-capture-ticks", "29,35,50,100,125",
                  "--eawr-live-input", f"20:click:unit={source}",
                  "--eawr-live-input", "30:key:X+alt",
                  "--eawr-live-input", f"35:click:unit={target}",
                  "--eawr-live-input", "40:key:T+shift",
                  "--eawr-live-input", f"45:click:unit={target}",
                  "--eawr-live-order", f"110:ability:{source}@ENERGY_WEAPON,off",
                  "--eawr-live-order", f"111:ability:{source}@TRACTOR_BEAM,off")
        code, result = self._run(directory, "beams", common, session=("--eawr-live-session", "replay"), camera=camera)
        self.assertEqual(code, 0, result.get("failure"))
        self.assertEqual(result["live_session"]["rejected"], [])
        self.assertEqual(result["live_session"]["ability_requests"], {"issued": 2, "refused": 0})
        self.assertEqual(result["battle_input"]["ability_bar"]["targeted"], 2, result["battle_input"]["log"])
        self.assertTrue(result["live_session"]["headless_hashes_equal"])
        effects = result["battle_effects"]
        self.assertGreater(effects["energy_beams_drawn"], 0, effects)
        self.assertGreater(effects["tractor_beams_drawn"], 0, effects)
        self.assertTrue(any(name.startswith("energy_owner:") and count > 0 for name, count in effects["spawned"].items()), effects)

    def test_tractor_line_width_texture_highlights_and_release(self):
        # TBF-01..03: inspect the mesh actually submitted, with a tractor-only source.
        directory = pathlib.Path(tempfile.mkdtemp(prefix="eawr-tractor-line-"))
        seed = directory / "seed.eawr-replay"
        fleet = ("--eawr-skirmish-slot", "1:Empire:0:human",
                 "--eawr-skirmish-slot", "2:Rebel:1:human",
                 "--eawr-skirmish-fleet", "1:Admonitor_Star_Destroyer",
                 "--eawr-skirmish-fleet", "2:Nebulon_B_Frigate",
                 "--eawr-live-ai", "off", "--eawr-live-player", "1",
                 "--eawr-live-step", "1", "--eawr-live-ticks", "8",
                 "--eawr-live-replay-out", str(seed))
        code, roster = self._run(directory, "roster", fleet, session=("--eawr-live-session", "skirmish"))
        self.assertEqual(code, 0, roster.get("failure"))
        source = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == "Admonitor_Star_Destroyer")
        target = next(row["entity"] for row in roster["live_session"]["start_fleet"] if row["type"] == "Nebulon_B_Frigate")
        replay = bytearray(seed.read_bytes())
        header = struct.unpack_from("<H", replay, 10)[0]
        players = struct.unpack_from("<I", replay, 48)[0]
        found = set()
        for index in range(struct.unpack_from("<Q", replay, 56)[0]):
            offset = header + players * 24 + index * 80
            entity = struct.unpack_from("<Q", replay, offset)[0]
            if entity not in (source, target):
                continue
            found.add(entity)
            struct.pack_into("<7q", replay, offset + 24,
                             (-400 if entity == source else 400) << 24, -1500 << 24, 0,
                             0, 0, 0, 1 << 24)
        self.assertEqual(found, {source, target})
        struct.pack_into("<Q", replay, 40, 140)
        staged = directory / "tractor.eawr-replay"
        staged.write_bytes(replay)
        # The rig returns text evidence; retain the derived setup for the lit motion eye-check.
        import base64
        (directory / "tractor-replay.txt").write_text(base64.b64encode(replay).decode("ascii"), encoding="ascii")
        camera = ROOT / "apps/viewer/project/config/coruscant-hero-beams-camera.xml"
        code, result = self._run(directory, "tractor", (
            "--eawr-live-replay", str(staged), "--eawr-live-reveal", "on", "--eawr-live-ai", "off",
            "--eawr-live-player", "1", "--eawr-live-step", "1", "--eawr-live-ticks", "125",
            "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
            "--eawr-live-capture-ticks", "29,50,60,80,120",
            "--eawr-live-input", f"20:click:unit={source}",
            "--eawr-live-input", "30:key:T+shift",
            "--eawr-live-input", f"35:click:unit={target}",
            "--eawr-live-order", f"100:ability:{source}@TRACTOR_BEAM,off"),
            session=("--eawr-live-session", "replay"), camera=camera)
        self.assertEqual(code, 0, result.get("failure"))
        self.assertEqual(result["live_session"]["rejected"], [])
        self.assertTrue(result["live_session"]["headless_hashes_equal"])
        effects = result["battle_effects"]
        self.assertGreater(effects["tractor_beams_drawn"], 0)
        self.assertEqual(effects["energy_beams_drawn"], 0)
        look = effects["hero_beam_looks"][1]
        self.assertEqual(look["frames"], 10)
        self.assertAlmostEqual(look["colour"][1], 75 / 255, places=5)
        self.assertEqual((look["colour"][0], look["colour"][2]), (0, 0))
        quad = look["last_quad"]
        self.assertAlmostEqual(math.dist(quad[0][:3], quad[1][:3]), 10, places=3)
        self.assertEqual([vertex[3:] for vertex in quad], [[0, 1], [1, 1], [0, 0], [1, 0]])
        self.assertEqual(look["sparks_drawn"], 4 * effects["tractor_beams_drawn"])
        self.assertGreater(look["moving_samples"], 3)
        self.assertEqual(look["active_sources"], 0, "release drops the source's highlight clock")

    def test_falcon_invulnerability_shows_authored_bubble_and_expires(self):
        # WHE-22/52: the active ordinary mode admits the model's authored pem proxies.
        with tempfile.TemporaryDirectory(prefix="eawr-falcon-mode-") as temporary:
            directory = pathlib.Path(temporary)
            fleet = ("--eawr-skirmish-slot", "1:Rebel:0:human",
                     "--eawr-skirmish-slot", "2:Empire:1:human",
                     "--eawr-skirmish-fleet", "1:Millennium_Falcon",
                     "--eawr-skirmish-fleet", "2:none", "--eawr-live-ai", "off",
                     "--eawr-live-reveal", "on", "--eawr-live-player", "1",
                     "--eawr-live-workers", "4", "--eawr-live-step", "1",
                     "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
            session = ("--eawr-live-session", "skirmish")
            code, idle = self._run(directory, "falcon-idle", (*fleet, "--eawr-live-ticks", "8"), session=session)
            self.assertEqual(code, 0, idle.get("failure"))
            falcon = next(row["entity"] for row in idle["live_session"]["start_fleet"]
                          if row["type"] == "Millennium_Falcon")
            self.assertFalse(any(name.lower().startswith("pem") for name in idle["unit_emitters"]["started"]))
            common = (*fleet, "--eawr-live-follow", str(falcon),
                      "--eawr-live-order", f"30:ability:{falcon}@INVULNERABILITY,on")
            code, active = self._run(directory, "falcon-active", (*common, "--eawr-live-ticks", "100"),
                                     session=session, camera=None)
            self.assertEqual(code, 0, active.get("failure"))
            self.assertEqual(active["live_session"]["rejected"], [])
            self.assertTrue(any(name.lower().startswith("pem") and count > 0
                                for name, count in active["unit_emitters"]["emitting"].items()))
            code, ended = self._run(directory, "falcon-expired", (*common, "--eawr-live-ticks", "360",
                                   "--eawr-live-capture-ticks", "29,45,100,211,250"), session=session, camera=None)
            self.assertEqual(code, 0, ended.get("failure"))
            emitters = ended["unit_emitters"]
            rows = [row for row in emitters["start_log"] if row["unit"] == falcon and row["proxy"].lower().startswith("pem")]
            self.assertTrue(rows, emitters)
            self.assertTrue(all(row["tick"] == 31 and row["first_age"] is not None for row in rows), rows)
            self.assertFalse(any(name.lower().startswith("pem") and count > 0 for name, count in emitters["emitting"].items()))
            self.assertEqual(emitters["invulnerability_drains"], {"started": len(rows), "finished": len(rows), "cut_short": 0})
            for report in (active, ended):
                self.assertTrue(report["live_session"]["headless_hashes_equal"])
                self.assertEqual(report["live_session"]["rejected"], [])

    def test_power_to_weapons_shows_hidden_glow_and_drains_on_end(self):
        # AB-32, BP-43: both Empire ships author two hidden pptw_ptwsa proxies. Switching the
        # ability must show it; off/expiration stops emission and drains (BP-48).
        with tempfile.TemporaryDirectory(prefix="eawr-live-power-glow-") as temporary:
            directory = pathlib.Path(temporary)
            common = ("--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                        "--eawr-live-player", "2", "--eawr-live-step", "1")
            for unit, duration in ((11, 210), (ACCLAMATOR, 600)):
                with self.subTest(unit=unit):
                    ship_view = (*common, "--eawr-live-follow", str(unit))
                    code, idle = self._run(directory, f"power-idle-{unit}",
                                           (*ship_view, "--eawr-live-ticks", "21"))
                    self.assertEqual(code, 0, idle.get("failure"))
                    self.assertNotIn("pptw_ptwsa", {name.lower() for name in idle["unit_emitters"]["started"]})
                    code, result = self._run(directory, f"power-ended-{unit}", (
                        *ship_view, "--eawr-live-order", f"30:ability:{unit}@POWER_TO_WEAPONS,on",
                        "--eawr-live-order", f"90:ability:{unit}@POWER_TO_WEAPONS,off",
                        # AB-12: enough time for either ship's proportional early-off recharge.
                        "--eawr-live-order", f"660:ability:{unit}@POWER_TO_WEAPONS,on",
                        "--eawr-live-ticks", str(660 + duration + 120)))
                    self.assertEqual(code, 0, result.get("failure"))
                    live, emitters = result["live_session"], result["unit_emitters"]
                    self.assertIs(live["headless_hashes_equal"], True)
                    self.assertEqual(live["rejected"], [])
                    self.assertIsNone(emitters["failure"])
                    self.assertEqual(emitters["start_failed"], {})
                    starts = {name.lower(): count for name, count in emitters["started"].items()}
                    self.assertEqual(starts.get("pptw_ptwsa"), 4, emitters)
                    self.assertFalse(emitters["start_log_full"], emitters)
                    rows = [row for row in emitters["start_log"]
                            if row["unit"] == unit and row["proxy"].lower() == "pptw_ptwsa"]
                    # The sample reads the newer snapshot and logs its completed tick.
                    self.assertEqual([row["tick"] for row in rows], [31, 31, 661, 661], rows)
                    self.assertTrue(all(row["first_age"] is not None for row in rows), rows)
                    drains = emitters["power_to_weapons_drains"]
                    self.assertEqual(drains, {"started": 4, "finished": 4, "cut_short": 0})

    def test_duel_fires_hits_and_explodes(self):
        # #80: the duel plays with the combat table bound; its shots draw as kites (Nebulon-B
        # turbolasers) and beams (Tartan lasers), hits spawn shield and detonation effects and the
        # Tartan's death its Death_Explosions, all from the snapshots: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-duel-") as temporary:
            directory = pathlib.Path(temporary)
            ticks = ",".join(str(tick) for tick in DUEL_CAPTURE_TICKS)
            code, result = self._run(directory, "duel", (
                *DUEL_ARGS, "--eawr-live-capture-ticks", ticks, "--eawr-live-ticks", "750"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fixture"], live["reveal"], live["local_player"]), ("replay", True, 2))
            # #494 FW-14: a revealed map draws no fog plane.
            self.assertEqual(result["live_fog"]["status"], "revealed")
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["hidden_units"], 0)
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertGreater(effects["max_kites"], 0)
            # BP-02: every kite's head (the short, glow-centred end) leads its flight.
            self.assertGreater(effects["kites_head_leading"], 0)
            self.assertEqual(effects["kites_head_trailing"], 0)
            self.assertGreater(effects["max_beams"], 0)
            self.assertEqual(effects["projectiles_not_drawn"], {})
            spawned = effects["spawned"]
            self.assertGreater(spawned.get("shield:Projectile_Shield_Absorb_Medium", 0)
                               + spawned.get("shield:Projectile_Shield_Absorb_Large", 0), 0, spawned)
            # #415 (BP-17 to BP-19): both duel ships have a SHIELD sub-object, and every shield hit
            # is placed where the shot's line first meets their collision meshes, facing that
            # triangle's normal. The stations have none.
            shields = effects["shield_hits"]
            for name in ("Nebulon_B_Frigate", "Tartan_Patrol_Cruiser"):
                self.assertIs(shields["meshes"][name]["shield"], True, shields)
                self.assertGreater(shields["meshes"][name]["triangles"], 0, shields)
            self.assertIs(shields["meshes"]["Skirmish_Rebel_Star_Base_1"]["shield"], False, shields)
            # A shot the collision box took that passes every mesh keeps the box contact and faces
            # back along the flight (BP-19); most meet a mesh.
            self.assertEqual(shields["no_mesh"], 0, shields)
            self.assertGreater(shields["on_mesh"], 3 * shields["mesh_missed"], shields)
            self.assertEqual(shields["on_mesh"] + shields["mesh_missed"],
                             sum(count for key, count in spawned.items() if key.startswith("shield:")), shields)
            # The cast starts at the projectile's frame-step start (BP-19): each mesh hit is met in
            # the step or ahead of it, one cast per shield hit, and the broad phase keeps the
            # triangles tested far below every triangle of every cast.
            self.assertEqual(shields["in_step"] + shields["ahead"], shields["on_mesh"], shields)
            casts = shields["casts"]
            self.assertEqual(casts["casts"], shields["on_mesh"] + shields["mesh_missed"], shields)
            largest = max(mesh["triangles"] for mesh in shields["meshes"].values())
            self.assertLess(casts["max_triangles_per_cast"], largest, shields)
            self.assertLess(casts["triangles_tested"], casts["casts"] * largest / 4, shields)
            for sample in shields["samples"]:
                self.assertAlmostEqual(sum(value * value for value in sample["direction"]), 1.0, places=2)
            self.assertTrue(any(sample["placed"] != sample["contact"] for sample in shields["samples"]), shields)
            # #438 (BP-21): retail shows no visible hull flash, so it stays off by default.
            # #76: without --eawr-live-defend the simulation's DEFEND drives the shell: the replay's
            # Rebel player is not human, so the Nebulon-B's script switches DEFEND on once the Tartan's
            # fire exceeds 20 a second (space-abilities AB-41; retail's frigate takes less, AB-U1).
            self.assertIs(live["defend"], False)
            self.assertIs(live["shield_flash"], False)
            self.assertEqual(live["shield_flashes"], 0)
            shells = result["populate"]["live_units"]["shield_shells"]
            self.assertEqual(shells, {"types": {"Nebulon_B_Frigate": "composed"}, "max_shown": 1})
            self.assertGreater(spawned.get("detonation:Large_Damage_Space", 0), 0, spawned)
            self.assertEqual(spawned.get("death:Large_Explosion_Space_Empire"), 1, spawned)
            self.assertEqual(effects["spawn_failed"], {})
            self.assertEqual(effects["effects_dropped"], 0)
            self.assertIs(effects["spawn_log_full"], False)
            for tick in DUEL_CAPTURE_TICKS:
                self.assertIn(f"duel_t{tick:04d}.png", result["captures"])
            # #370 review 1: every hit the player saw spawned its impact, tick by tick, the
            # Tartan's fatal hit included (its target left the session that same tick).
            hits = {int(tick): count for tick, count in effects["hit_events"].items()}
            self.assertEqual(hit_rows_per_tick(effects), hits)
            deaths = [row for row in effects["spawn_log"] if row[1] == "death:Large_Explosion_Space_Empire"]
            self.assertEqual([row[0] for row in deaths], [DUEL_DEATH_TICK])
            self.assertGreaterEqual(hit_rows_per_tick(effects).get(DUEL_DEATH_TICK, 0), 1, effects["hit_events"])
            # #370 review 2: at --eawr-live-step 3 a frame presenting tick 3k reaches ticks 3k-1,
            # 3k and 3k+1; an effect of tick t is born at presented tick t - 1, so it is first
            # drawn at age 2, 1 or 0, the last reached tick's at 0.
            ages = {}
            for tick, key, first_age in effects["spawn_log"]:
                self.assertIsNotNone(first_age, (tick, key))
                self.assertEqual(first_age, (1 - tick) % 3, (tick, key))
                ages[first_age] = ages.get(first_age, 0) + 1
            self.assertEqual(sorted(ages), [0, 1, 2], ages)


    def test_missiles_fly_as_models_with_their_trails(self):
        # #456 (BP-60 to BP-62): each concussion missile flies on a model slot of its type's pool,
        # and its p_concussion trail starts once for its whole flight, the catch-up samples of a
        # three-tick step included. This replay's missiles name no hit particle list (BP-63).
        with tempfile.TemporaryDirectory(prefix="eawr-live-missiles-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "missiles", (
                "--eawr-live-replay", str(S28), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                "--eawr-live-step", "3", "--eawr-live-ticks", "300"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertEqual(effects["projectiles_not_drawn"], {})
            pool = effects["projectile_models"]["Proj_Ship_Concussion_Missile"]
            self.assertEqual((pool["slots"], pool["refused"]), (32, 0), pool)
            self.assertGreater(pool["bindings"], 0, pool)
            self.assertGreater(effects["projectile_models_drawn"], 0)
            self.assertGreater(effects["spawned"].get("detonation:Conc_Missile_Detonation_Particle", 0), 0)
            self.assertEqual(effects["hit_picks"], {"by_projectile": 0, "by_event": 0})
            units = result["populate"]["live_units"]
            # BP-66: the ion override has its own prepared pool, even when this replay never fires it.
            ion_pool = effects["projectile_models"]["Proj_Ion_Cannon_Medium_Laser_Blue"]
            self.assertEqual((ion_pool["slots"], ion_pool["bindings"], ion_pool["refused"]), (32, 0, 0), ion_pool)
            # The higher-level production closure also preloads the diamond-boron model.
            diamond_pool = effects["projectile_models"]["Proj_Ship_Diamond_Boron_Missile"]
            self.assertEqual((diamond_pool["slots"], diamond_pool["bindings"], diamond_pool["refused"]),
                             (32, 0, 0), diamond_pool)
            # The loaded production closure also prepares pad buildables (#1009), Barrage
            # overrides (#1307), and spawned hero projectiles (#1426), even when dormant.
            self.assertEqual(set(effects["projectile_models"]), {
                "Proj_Ion_Cannon_Medium_Laser_Blue", "Proj_Plasma_Space_Turret_Blast",
                "Proj_Ship_Concussion_Missile", "Proj_Ship_Concussion_Missile_Satellite",
                "Proj_Ship_Diamond_Boron_Missile", "Proj_Ship_Diamond_Boron_Missile_Barrage",
                "Proj_Harmonic_Bomb_Slave_I"})
            for name in ("Proj_Plasma_Space_Turret_Blast", "Proj_Ship_Concussion_Missile_Satellite",
                         "Proj_Ship_Diamond_Boron_Missile_Barrage", "Proj_Harmonic_Bomb_Slave_I"):
                extra_pool = effects["projectile_models"][name]
                self.assertEqual((extra_pool["slots"], extra_pool["bindings"], extra_pool["refused"]),
                                 (32, 0, 0), extra_pool)
            self.assertEqual(units["projectile_slots_drawn"], 224)
            undrawn = units["projectile_slots_not_drawn"]
            self.assertEqual(undrawn, [])
            self.assertEqual(result["unit_emitters"]["started"].get("p_concussion"), pool["bindings"],
                             result["unit_emitters"]["started"])


    def test_missile_trails_reach_their_true_last_tick_at_every_step(self):
        # #491: a catch-up sample must answer from the binding valid at the historical tick it
        # is sampling, not the one this frame's single bind() call just computed - or a missile
        # ending between two frames loses the last tick(s) of its trail to a slot bind() already
        # freed. --eawr-live-step only paces how the fixed replay is presented; at step 1 a
        # frame never reaches more than one new tick, so the catch-up loop never runs and
        # "projectile_model_last_tick" (BattleEffects, keyed by projectile ID) is exactly the
        # sim's own truth. Any --eawr-live-step > 1 must compute the identical value per missile;
        # an aggregate count (bindings, projectile_models_drawn) cannot see a trail cut short by
        # one or two ticks, so this compares the per-projectile last tick itself.
        last_tick_by_step = {}
        for step in (1, 2, 3, 5):
            with tempfile.TemporaryDirectory(prefix=f"eawr-live-missile-step{step}-") as temporary:
                directory = pathlib.Path(temporary)
                code, result = self._run(directory, f"missile_step{step}", (
                    "--eawr-live-replay", str(S28), "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                    "--eawr-live-step", str(step), "--eawr-live-ticks", str(S28_LIVE_TICKS)),
                    session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                effects = result["battle_effects"]
                self.assertIsNone(effects["failure"])
                pool = effects["projectile_models"]["Proj_Ship_Concussion_Missile"]
                self.assertEqual(pool["refused"], 0, pool)
                self.assertGreater(pool["bindings"], 0, pool)
                # Only missiles the --eawr-live-ticks window itself reached: the simulation thread
                # keeps ticking after the presented view holds there, so a run can see a later
                # missile the window never actually presented (not an #491 symptom).
                last_tick_by_step[step] = {int(key): value for key, value in effects["projectile_model_last_tick"].items()
                                            if value <= S28_LIVE_TICKS}
        baseline = last_tick_by_step[1]
        self.assertTrue(baseline, "step 1 (no catch-up) should have posed at least one missile")
        for step, last_tick in last_tick_by_step.items():
            self.assertEqual(last_tick, baseline,
                              f"--eawr-live-step {step} must pose every missile's trail to the exact same "
                              "last tick as step 1, tick by tick, not just in aggregate (#491)")


    def test_squadron_selection_plays_its_craft_lines(self):
        # #499 (docs/behaviour/battle-audio.md BA-24): a squadron's team container has no response
        # sounds or ranking category of its own (every M2 squadron); the FoC debug build resolves a
        # selected squadron to its leading live craft for both the unit-response ranking and the
        # sound played. Before the fix the container's own (missing) fields meant a selected
        # squadron played no line at all -- the owner's "no voice lines for fighters/bombers yet".
        # #424, tests/presentation/renderer/test_battle_input.py: the M2 start (sim_headless
        # --skirmish m2) Rebel X-wing squadron 2, Y-wing squadron 4, Empire TIE Interceptor
        # squadron 9; clicking a squadron's icon selects it regardless of where its craft draw.
        # Every Rebel company starts stacked on the same spawn marker, so their icons coincide on
        # screen and a click there hits whichever the pointer resolution finds first; SPREAD (test_
        # battle_input.py) moves the corvette and both X-wing squadrons apart first, leaving the
        # Y-wing squadron and the MC80 (unit 7) at the stack; the icon takes the click first (WU-23).
        x_wing_squadron, y_wing_squadron, tie_squadron = 2, 4, 9
        spread = ("--eawr-live-order", "1:move:5@-4450,5050,0", "--eawr-live-order", "1:move:2@-5450,5250,0",
                  "--eawr-live-order", "1:move:3@-4650,4250,0")
        with tempfile.TemporaryDirectory(prefix="eawr-live-squadron-audio-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "rebel-squadrons", (
                *spread, "--eawr-hud", "off", "--eawr-live-ticks", "90",
                "--eawr-live-input", f"30:click:icon={x_wing_squadron}",
                "--eawr-live-input", f"60:click:icon={y_wing_squadron}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["responses"], [
                "select X-Wing Unit_Select_X_Wing",
                "select Y-Wing Unit_Select_Y_Wing",
            ])
            self.assertEqual(sound["results"].get("response_select"), {"playing": 2}, sound["results"])

            # The Empire player's view: TIE Interceptor squadron 9 stands at its own spawn, far from
            # the Rebel-side default camera, so this run points at it instead (coruscant-empire-
            # live-session-camera.xml).
            empire_camera = ROOT / "apps/viewer/project/config/coruscant-empire-live-session-camera.xml"
            code, result = self._run(directory, "empire-squadron", (
                "--eawr-hud", "off", "--eawr-live-ticks", "60", "--eawr-live-player", "2",
                "--eawr-live-input", f"30:click:icon={tie_squadron}"), camera=empire_camera)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            sound = result["battle_audio"]
            self.assertEqual(sound["responses"], ["select TIE_Interceptor Unit_Select_Tie_Interceptor"])
            self.assertEqual(sound["results"].get("response_select"), {"playing": 1}, sound["results"])


    def test_turbo_engine_boost_fades_out(self):
        # #559 (battle-presentation BP-65): when TURBO ends the corvette's turbo engine emitters stop
        # emitting and their particles drain instead of vanishing at once, and the normal engine
        # emitters that stood in for them drain the same way when TURBO starts. Corvette 5 of the M2
        # start is switched on at tick 50 and off at tick 170 by script.
        with tempfile.TemporaryDirectory(prefix="eawr-live-turbo-drain-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "turbo", (
                "--eawr-live-order", "2:move:5@4000,4700,0", "--eawr-live-order", "50:ability:5@TURBO,on",
                "--eawr-live-order", "170:ability:5@TURBO,off", "--eawr-live-ticks", "330"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            drains = result["unit_emitters"]["engine_drains"]
            self.assertGreaterEqual(drains["started"], 2, drains)
            self.assertEqual(drains["cut_short"], 0, drains)
            self.assertEqual(drains["finished"], drains["started"], drains)
            self.assertIn("PTE_Corvetteengines", result["unit_emitters"]["started"])


    def test_shield_flash_opt_in(self):
        # #438: preserve the debug-build BP-21 light-scale path as an explicit viewer option.
        with tempfile.TemporaryDirectory(prefix="eawr-live-shield-flash-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "flash", (
                *DUEL_ARGS, "--eawr-live-shield-flash", "on", "--eawr-live-ticks", "750"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["shield_flash"], True)
            self.assertGreater(live["shield_flashes"], 0)
            self.assertIs(live["headless_hashes_equal"], True)


    def test_defend_shows_the_shield_shell(self):
        # #427 (BP-22 to BP-24): the Nebulon-B (the duel's DEFEND type; the MC80 is not in S-15) shows its SHIELD
        # sub-object with its MeshShield.fx material while DEFEND runs; --eawr-live-defend forces it
        # on. The simulation's DEFEND (#76) can switch on at tick 30 at the earliest (its damage-rate
        # window, space-abilities AB-42), and since #536 the Tartan's lasers land about 8 damage at a
        # time, as in the S-15 recording, so the frigate's own DEFEND waits for the scripted tick-500
        # hit (AB-U1): only the forced shell shows. The shell is presentation only: the hashes stay
        # headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-defend-") as temporary:
            directory = pathlib.Path(temporary)
            frames = {}
            for name, defend in (("defend", "on"), ("plain", "off")):
                code, result = self._run(directory, name, (
                    *DUEL_ARGS, "--eawr-live-defend", defend, "--eawr-live-capture-ticks", "24",
                    "--eawr-live-ticks", "24"), session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertIs(live["defend"], defend == "on")
                self.assertIs(live["headless_hashes_equal"], True)
                shells = result["populate"]["live_units"]["shield_shells"]
                self.assertEqual(shells, {"types": {"Nebulon_B_Frigate": "composed"},
                                          "max_shown": 1 if defend == "on" else 0})
                frames[name] = decode_png((directory / f"{name}_t0024.png").read_bytes())
            self.assertNotEqual(frames["defend"][2], frames["plain"][2])


    def test_human_default_autofire_shows_defend_shell(self):
        # AB-41/45, BP-22: a human's fresh-profile default arms DEFEND. Damage activates
        # the real simulation ability; an explicit autofire-off command prevents it.
        with tempfile.TemporaryDirectory(prefix="eawr-live-autofire-") as temporary:
            directory = pathlib.Path(temporary)
            frames = {}
            for name, orders, shown in (("default", (), 1),
                    ("manual", ("--eawr-live-order", f"0:ability:{NEBULON}@DEFEND,manual"), 0)):
                code, result = self._run(directory, name, (
                    "--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-reveal", "on",
                    "--eawr-live-follow", str(NEBULON), "--eawr-live-step", "1", *orders,
                    "--eawr-live-order", f"10:damage:{NEBULON}@100", "--eawr-live-ticks", "45",
                    "--eawr-live-capture-ticks", "36", "--eawr-environment", "map", "--eawr-lighting", "sh"))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["defend"], False)
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                self.assertEqual(result["populate"]["live_units"]["shield_shells"]["max_shown"], shown)
                frames[name] = decode_png((directory / f"{name}_t0036.png").read_bytes())
            self.assertNotEqual(frames["default"][2], frames["manual"][2])


    def test_duel_stall_keeps_every_event(self):
        # #370 review 3: a frame after a presentation stall reaches far more ticks than the
        # snapshot history keeps (64). The session's event log still hands it every tick's
        # hits and deaths: the Tartan's fatal hit, its death explosion and its death clone all
        # arrive, with the same hits as the unstalled run, and no event gap is reported.
        with tempfile.TemporaryDirectory(prefix="eawr-live-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "600:150"))):
                code, result = self._run(directory, name, (*DUEL_ARGS, "--eawr-live-ticks", "750", *extra),
                                         session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                self.assertEqual(result["live_session"]["event_gaps"], [])
                self.assertIsNone(result["battle_effects"]["failure"])
                self.assertEqual(result["battle_effects"]["effects_dropped"], 0)
                runs[name] = result
            stalled = runs["stalled"]
            self.assertEqual(stalled["live_session"]["stall"], {"tick": 600, "ticks": 150})
            # The frame after tick 600 presents 753: ticks 602 to 754 arrive at once.
            self.assertEqual(stalled["live_session"]["latest_tick"], 754)
            hits = {int(tick): count for tick, count in stalled["battle_effects"]["hit_events"].items()}
            paced = {int(tick): count for tick, count in runs["paced"]["battle_effects"]["hit_events"].items()}
            self.assertTrue(any(601 < tick < 690 for tick in paced), paced)
            self.assertEqual({tick: count for tick, count in hits.items() if tick <= 751}, paced)
            self.assertEqual(hit_rows_per_tick(stalled["battle_effects"]), hits)
            # The death (born at presented tick 717) is 36 ticks old when the frame presenting 753
            # reaches it, past its 30-frame lifetime: it is skipped, not spawned to drain at once
            # (#370 re-review 2), and stays in the spawn log without a drawn age.
            deaths = [row for row in stalled["battle_effects"]["spawn_log"]
                      if row[1] == "death:Large_Explosion_Space_Empire"]
            self.assertEqual(deaths, [[DUEL_DEATH_TICK, "death:Large_Explosion_Space_Empire", None]])
            self.assertEqual(stalled["battle_effects"]["expired"].get("death:Large_Explosion_Space_Empire"), 1)
            self.assertIsNone(stalled["battle_effects"]["spawned"].get("death:Large_Explosion_Space_Empire"))
            self.assertGreaterEqual(hits.get(DUEL_DEATH_TICK, 0), 1)
            clones = stalled["live_session"]["death_clones_shown"] + stalled["live_session"]["death_clones_retired"]
            self.assertEqual([(row["unit"], row.get("death_tick")) for row in clones], [(2, DUEL_DEATH_TICK)])
            # A capture tick inside the skip is refused.
            code, result = self._run(directory, "skipped", (
                *DUEL_ARGS, "--eawr-live-stall", "600:150", "--eawr-live-capture-ticks", "690"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertNotEqual(code, 0)
            self.assertIn("falls in the --eawr-live-stall skip", result["failure"])


    def test_duel_startup_stall_ages_every_effect(self):
        # #370 re-review 2: the session reaches tick 150 before the viewer's first frame. That
        # frame fires the hits of ticks 1 to 151 at once; each effect is born at its own tick
        # (presented tick - 1) and aged by the ticks since, so it is first drawn at age
        # 151 - tick, and one whose Particle_Lifetime_Frames are already over is skipped rather
        # than spawned with the rest at age zero.
        with tempfile.TemporaryDirectory(prefix="eawr-live-startup-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "startup", (
                *DUEL_ARGS, "--eawr-live-ticks", "300", "--eawr-live-stall", "start:150"),
                session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["stall"], {"tick": "start", "ticks": 150})
            self.assertEqual(live["event_gaps"], [])
            effects = result["battle_effects"]
            self.assertIsNone(effects["failure"])
            self.assertEqual(effects["effects_dropped"], 0)
            hits = {int(tick): count for tick, count in effects["hit_events"].items()}
            self.assertEqual(hit_rows_per_tick(effects), hits)
            types = effects["particle_types"]
            aged = expired = 0
            for tick, key, first_age in effects["spawn_log"]:
                if tick > 151:
                    continue
                lifetime = types[key.split(":", 1)[1]]["lifetime_frames"]
                self.assertGreater(lifetime, 0, key)
                if 151 - tick >= lifetime:
                    self.assertIsNone(first_age, (tick, key))
                    expired += 1
                else:
                    self.assertEqual(first_age, 151 - tick, (tick, key))
                    aged += first_age > 0
            # The duel fires before tick 150: some early effects are still alive, some over.
            self.assertGreater(aged, 0, effects["spawn_log"][:20])
            self.assertGreater(expired, 0, effects["spawn_log"][:20])
            self.assertEqual(sum(effects["expired"].values()), expired)


    def test_unit_emitters_after_a_stall(self):
        # #406 review: a frame after a presentation stall runs the samples it catches up on at
        # their own ticks. The Nebulon-B moves and loses its FL hardpoint at tick 45, inside a
        # stall that jumps from tick 30 to 90.5 (the default step is half a tick): its damage
        # emitter is born at the tick whose state shows the hardpoint destroyed, where the moving
        # ship stood then (the paced run's birth sample and origin), and is first drawn aged from
        # there, not from the frame's start with the whole skip at the final pose.
        orders = ("--eawr-live-order", f"15:move:{NEBULON}@-4400,5400,0",
                  "--eawr-live-order", f"45:damage:{NEBULON}@5000,{NEBULON_FL}")
        damage = "p_hp_stardestroyer_damage"
        with tempfile.TemporaryDirectory(prefix="eawr-live-emitters-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "30:60"))):
                code, result = self._run(directory, name, ("--eawr-live-ticks", "120", *orders, *extra))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                emitters = result["unit_emitters"]
                self.assertIsNone(emitters["failure"])
                self.assertEqual(emitters["start_failed"], {})
                self.assertEqual(emitters["started"].get(damage), 1, emitters["started"])
                self.assertIs(emitters["start_log_full"], False)
                runs[name] = emitters
            paced = {(row["unit"], row["proxy"], row["tick"]): row for row in runs["paced"]["start_log"]}
            stalled = runs["stalled"]
            # Samples 30 to 88 run before the frame's own (89, due 90).
            self.assertEqual(stalled["caught_up"], 59)
            self.assertEqual(stalled["unknown_samples"], 0)
            # A paced frame draws a new emitter after its one sample.
            self.assertTrue(all(row["first_age"] == 1 for row in paced.values() if row["proxy"] == damage), paced)
            rows = [row for row in stalled["start_log"] if row["proxy"] == damage]
            self.assertEqual(len(rows), 1, stalled["start_log"])
            row = rows[0]
            self.assertTrue(31 < row["tick"] <= 90, row)
            match = paced.get((row["unit"], row["proxy"], row["tick"]))
            self.assertIsNotNone(match, (row, sorted(paced)))
            self.assertEqual(row["born"], match["born"], (row, match))
            # First drawn by the frame presenting 90.5 (due 90): aged by the samples since its
            # birth, as the paced run's emitter is at that tick.
            self.assertEqual(row["first_age"], 90 - row["born"], row)
            for axis in range(3):
                self.assertAlmostEqual(row["origin"][axis], match["origin"][axis], delta=0.5, msg=(row, match))
            # Past the snapshot history (a first-frame stall to tick 200, the kill at 45): where
            # the ship stood during the older ticks is unknown, so no emitter runs over them
            # (least visible); the damage emitter starts at the oldest tick the history holds.
            code, result = self._run(directory, "startup", (
                "--eawr-live-ticks", "260", *orders, "--eawr-live-stall", "start:200"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertGreater(emitters["unknown_samples"], 0)
            rows = [row for row in emitters["start_log"] if row["proxy"] == damage]
            self.assertEqual(len(rows), 1, emitters["start_log"])
            self.assertGreater(rows[0]["tick"], 200 - 66, rows[0])
            self.assertEqual(rows[0]["first_age"], 200 - rows[0]["tick"] + 2, rows[0])


    def test_corvette_glow_spans_nozzle_wall_during_turn(self):
        # BP-40/PS-10/PS-15: later every-vertex births retain the full nozzle wall.
        # Compare the glow's principal extent with the independently drawn blue
        # nozzle rims, allowing foreshortening and the soft sprite perimeter.
        from test_area_damage_capture import point_height_replay
        with tempfile.TemporaryDirectory(prefix="eawr-nozzle-wall-") as temporary:
            directory = pathlib.Path(temporary)
            data = bytearray(point_height_replay((ROOT / "tests/skirmish/fixtures/m2-start.eawr-replay").read_bytes()))
            header = struct.unpack_from("<H", data, 10)[0]
            players = struct.unpack_from("<I", data, 48)[0]
            start = header + players * 24
            for index in range(2):
                offset = start + index * 80
                entity = struct.unpack_from("<Q", data, offset)[0]
                self.assertIn(entity, (5, 11))
                struct.pack_into("<7q", data, offset + 24,
                                 (0 if entity == 5 else 8000) << 24, 0, 0, 0, 0, 0, 1 << 24)
            struct.pack_into("<Q", data, 64, 0)
            replay = directory / "nozzles.eawr-replay"
            replay.write_bytes(data[:start + 160])
            camera = directory / "camera.xml"
            config = CAMERA.read_text(encoding="utf-8")
            config = re.sub(r"<initial[^>]+/>",
                '<initial target_x="0" target_y="0" target_height="0" zoom="0.10" yaw_degrees="270"/>', config)
            config = re.sub(r'(<override tag="Pitch_Min" value=")[^"]+', r'\g<1>10', config)
            camera.write_text(config, encoding="utf-8")
            shutil.copyfile(CAMERA.parent / "space-live-camera-bindings.json",
                            directory / "space-live-camera-bindings.json")
            images = {}
            for name, orders in (("rest", ()), ("turn", ("--eawr-live-order", "2:face:5@0,1000,0"))):
                code, result = self._run(directory, name, (
                    "--eawr-live-replay", str(replay), "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                    "--eawr-live-step", "1", "--eawr-live-ticks", "100", "--eawr-live-capture-ticks", "60",
                    "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                    "--eawr-hud", "off", *orders), session=("--eawr-live-session", "replay"),
                    camera=camera, engine_args=("--fixed-fps", "30"))
                self.assertEqual(code, 0, result.get("failure"))
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                self.assertEqual(result["unit_emitters"]["start_failed"], {})
                images[name] = decode_png((directory / f"{name}_t0060.png").read_bytes())

            def points(image, region, predicate):
                width, height, rows = image
                return [(x, y) for y in range(int(height * region[1]), int(height * region[3]))
                        for x in range(int(width * region[0]), int(width * region[2])) if predicate(rows[y][x])]

            def extent(cloud):
                self.assertGreater(len(cloud), 100, "visible nozzle/glow pixels are required")
                mx = sum(x for x, _ in cloud) / len(cloud)
                my = sum(y for _, y in cloud) / len(cloud)
                xx = sum((x - mx) ** 2 for x, _ in cloud)
                yy = sum((y - my) ** 2 for _, y in cloud)
                xy = sum((x - mx) * (y - my) for x, y in cloud)
                angle = 0.5 * math.atan2(2 * xy, xx - yy)
                projected = sorted(x * math.cos(angle) + y * math.sin(angle) for x, y in cloud)
                return projected[int(len(projected) * 0.99)] - projected[int(len(projected) * 0.01)]

            rim = extent(points(images["rest"], (0.44, 0.60, 0.56, 0.70),
                                lambda p: p[2] > 60 and p[2] > p[0] * 1.08 and p[2] > p[1] * 1.10))
            for name, image in images.items():
                glow = extent(points(image, (0.42, 0.53, 0.65, 0.72),
                                     lambda p: p[0] > 100 and p[0] > p[1] * 1.25 and p[0] > p[2] * 2))
                self.assertGreater(glow / rim, 0.70, (name, "glow/nozzle extent", glow / rim))

    def test_engine_emitters_are_drawn_every_frame_of_a_turn(self):
        # #433: the corvette turns sharply (faced away, then ordered to its left) at the default
        # half-tick step, so every other frame falls between two 30 Hz emitter samples. Those
        # frames draw its engine emitters again at the pose drawn then, and each engine emitter
        # runs for the whole turn: started once, never stopped and started again. The start log
        # counts the frames between samples that drew each started emitter again (presented), so
        # the engine's own count shows it was drawn between samples through the turn (#439).
        # Presentation only: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-glow-turn-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "turn", (
                "--eawr-live-ticks", "150",
                "--eawr-live-order", f"2:face:{CORVETTE}@-5739,3969,0",
                "--eawr-live-order", f"60:move:{CORVETTE}@-3229,2995,0"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertEqual(emitters["start_failed"], {})
            self.assertNotIn("frame_overflow", emitters["stopped"])
            self.assertIs(emitters["start_log_full"], False)
            # PE_Corvetteengines is the corvette's only engine emitter.
            engines = [row for row in emitters["start_log"]
                       if row["unit"] == CORVETTE and row["proxy"].lower() == "pe_corvetteengines"]
            self.assertEqual(len(engines), 1, emitters["start_log"])
            # One frame in two falls between samples; the engine is drawn on each of them from
            # its start to the end of the session, the turn included.
            self.assertGreater(engines[0]["presented"], 0, engines[0])
            self.assertGreaterEqual(engines[0]["presented"], emitters["frames"] // 2 - engines[0]["tick"] - 4,
                                    (engines[0], emitters["frames"]))
            self.assertGreaterEqual(emitters["presented_frames"], engines[0]["presented"], emitters)
            self.assertGreaterEqual(emitters["presented_effects"], emitters["presented_frames"], emitters)


    def test_generator_pulse_follows_hardpoint_state(self):
        args = ("--eawr-skirmish-slot", "1:Underworld:0:human",
                "--eawr-skirmish-slot", "2:Empire:1:human",
                "--eawr-skirmish-fleet", "1:none", "--eawr-skirmish-fleet", "2:none",
                "--eawr-live-ai", "off", "--eawr-live-reveal", "on",
                "--eawr-live-step", "1", "--eawr-live-ticks", "90")
        with tempfile.TemporaryDirectory(prefix="eawr-attached-emitter-") as temporary:
            directory = pathlib.Path(temporary)
            camera = directory / "generator.xml"
            text = CAMERA.read_text(encoding="utf-8")
            for tag, value in (("target_x", -3811), ("target_y", 4460)):
                text = re.sub(rf'{tag}="[^"]+"', f'{tag}="{value}"', text)
            camera.write_text(text, encoding="utf-8")
            shutil.copyfile(CAMERA.parent / "space-live-camera-bindings.json",
                            directory / "space-live-camera-bindings.json")
            for name, orders, alive in (
                ("intact", (), True),
                ("damaged", ("--eawr-live-order", "30:damage:1@1,3"), True),
                ("destroyed", ("--eawr-live-order", "30:damage:1@1000000,3"), False),
                ("retired", ("--eawr-live-order", "30:damage:1@1000000"), False),
            ):
                with self.subTest(name=name):
                    code, result = self._run(directory, name, (*args, *orders),
                                             session=("--eawr-live-session", "skirmish"), camera=camera)
                    self.assertEqual(code, 0, result.get("failure"))
                    self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                    self.assertEqual(result["live_session"]["rejected"], [])
                    emitters = result["unit_emitters"]
                    self.assertIsNone(emitters["failure"])
                    self.assertEqual(emitters["start_failed"], {})
                    for proxy in ("p_u_shieldgen_pulse", "p_u_shieldgen"):
                        # The destroyed hardpoint's breakoff model has its own root proxies.
                        # Check the station's attached instance separately from that debris.
                        active = emitters["emitting_by_unit"].get("1", {})
                        self.assertEqual(active.get(proxy, 0), int(alive), emitters)
                        rows = [row for row in emitters["start_log"]
                                if row["unit"] == 1 and row["proxy"] == proxy]
                        self.assertEqual(len(rows), 1)
                        self.assertEqual(rows[0]["unit"], 1)
                        self.assertIsNotNone(rows[0]["origin"])
                        self.assertIsNotNone(rows[0]["first_age"])
                        # The station stands away from the map origin: attached emitters ride it.
                        self.assertGreater(sum(v * v for v in rows[0]["origin"][:2]), 1000000)



    def test_kite_axis_matches_projected_flight_over_frames(self):
        import math
        import struct

        fleet = ("--eawr-skirmish-slot", "1:Rebel:0:human",
                 "--eawr-skirmish-slot", "2:Empire:1:human",
                 "--eawr-skirmish-fleet", "1:Calamari_Cruiser",
                 "--eawr-skirmish-fleet", "2:Acclamator_Assault_Ship",
                 "--eawr-live-ai", "off", "--eawr-live-reveal", "on")
        with tempfile.TemporaryDirectory(prefix="eawr-projectile-axis-") as temporary:
            directory = pathlib.Path(temporary)
            # Resolve the selected fleet's identities from this install, independently of
            # the pinned fidelity replays and default roster ordering.
            replay = directory / "axis.eawr-replay"
            code, initial = self._run(directory, "fleet", (*fleet, "--eawr-live-ticks", "8",
                                      "--eawr-live-replay-out", str(replay)),
                                      session=("--eawr-live-session", "skirmish"))
            self.assertEqual(code, 0, initial.get("failure"))
            units = initial["live_session"]["start_fleet"]
            shooter = next(row["entity"] for row in units if row["type"] == "Calamari_Cruiser")
            target = next(row["entity"] for row in units if row["type"] == "Acclamator_Assault_Ship")
            # Stage the selected ships directly in a fresh recorded setup. Navigating
            # from opposing spawn markers can leave them outside the camera and range.
            # Only temporary replay bytes change; the installed content identity and
            # four committed pins stay intact. Layout: docs/replay-format.md.
            data = bytearray(replay.read_bytes())
            self.assertEqual(data[:8], b"EAWRPLY\0")
            self.assertIn(struct.unpack_from("<H", data, 8)[0], (2, 3, 4, 5))
            header = struct.unpack_from("<H", data, 10)[0]
            players = struct.unpack_from("<I", data, 48)[0]
            count = struct.unpack_from("<Q", data, 56)[0]
            self.assertEqual(struct.unpack_from("<Q", data, 64)[0], 0)
            self.assertLessEqual(header + 24 * players + 80 * count, len(data))
            quarter = round(math.sqrt(0.5) * (1 << 24))
            staged = set()
            for index in range(count):
                offset = header + 24 * players + 80 * index
                entity = struct.unpack_from("<Q", data, offset)[0]
                if entity in (shooter, target):
                    y, w = (1500, quarter) if entity == shooter else (2300, -quarter)
                    struct.pack_into("<3q4q", data, offset + 24,
                                     -2200 * (1 << 24), y * (1 << 24), 0, 0, 0, quarter, w)
                    staged.add(entity)
            self.assertEqual(staged, {shooter, target})
            struct.pack_into("<Q", data, 40, 360)
            struct.pack_into("<Q", data, 64, 2)
            for player, unit, other in ((1, shooter, target), (2, target, shooter)):
                body = struct.pack("<QIQBBH", 1, player, 1, 3, 0, 0)
                body += struct.pack("<QIIQ", other, 1, 0, unit)
                data += struct.pack("<I", len(body)) + body
            replay.write_bytes(data)
            shutil.copyfile(DUEL_CAMERA.parent / "space-live-camera-bindings.json",
                            directory / "space-live-camera-bindings.json")
            for name, target_x, target_y, yaw in (
                ("centred", -2200, 1900, 0),
                ("off-centre", -1900, 2200, 35),
                ("opposite-edge", -2500, 1700, -35),
            ):
                with self.subTest(name=name):
                    camera = directory / f"{name}.xml"
                    text = DUEL_CAMERA.read_text(encoding="utf-8")
                    for tag, value in (("target_x", target_x), ("target_y", target_y), ("yaw_degrees", yaw)):
                        text = re.sub(rf'{tag}="[^"]+"', f'{tag}="{value}"', text)
                    camera.write_text(text, encoding="utf-8")
                    code, result = self._run(directory, name,
                        ("--eawr-live-replay", str(replay), "--eawr-live-reveal", "on",
                         "--eawr-live-step", "0.5", "--eawr-live-ticks", "180"),
                        session=("--eawr-live-session", "replay"), camera=camera)
                    self.assertEqual(code, 0, result.get("failure"))
                    self.assertIs(result["live_session"]["headless_hashes_equal"], True)
                    self.assertEqual(result["live_session"]["rejected"], [])
                    measured = result["battle_effects"]["kite_axis"]
                    self.assertGreater(measured["samples"], 100, measured)
                    self.assertLess(measured["max_sine_error"], 0.003, measured)
                    self.assertEqual(measured["reversed"], 0, measured)


    def test_effective_constants_change_drawn_width_and_flash(self):
        from tools.inventory.corpus import Corpus
        import xml.etree.ElementTree as ET
        corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
        constants = corpus.read_effective("foc", "data/xml/gameconstants.xml")
        projectiles = corpus.read_effective("foc", "data/xml/projectiles.xml")
        self.assertIsNotNone(constants)
        self.assertIsNotNone(projectiles)
        with tempfile.TemporaryDirectory(prefix="eawr-presentation-constants-") as temporary:
            directory = pathlib.Path(temporary)
            results = {}
            variants = {
                "baseline": {},
                "beam_zero": {"Laser_Beam_Z_Scale_Factor": "0"},
                "kite_zero": {"Laser_Kite_Z_Scale_Factor": "0"},
                "flash_scale": {"Shield_Flash_Scale": "3,4,5"},
                "flash_duration": {"Shield_Flash_Duration": "1"},
                "flash_zero": {"Shield_Flash_Duration": "0"},
            }
            for name, changes in variants.items():
                xml = directory / name / "Data/XML"
                xml.mkdir(parents=True)
                (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
                root = ET.fromstring(constants.data)
                for tag, value in changes.items():
                    node = next(child for child in root if child.tag.lower() == tag.lower())
                    node.text = value
                ET.ElementTree(root).write(xml / "GameConstants.xml", encoding="utf-8")
                # Exercise mod integer spellings through the catalog and the production loader.
                root = ET.fromstring(projectiles.data)
                for node in root.iter("Projectile_Custom_Render"):
                    if node.text and node.text.strip() in ("1", "2"):
                        node.text = "01" if node.text.strip() == "1" else "2.0"
                ET.ElementTree(root).write(xml / "Projectiles.xml", encoding="utf-8")
                code, result = self._run(directory, name, (
                    *DUEL_ARGS, "--eawr-mod-root", str(xml.parent.parent), "--eawr-live-ticks", "750",
                    "--eawr-live-step", "1", "--eawr-live-shield-flash", "on",
                    "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on"),
                    session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                results[name] = result
            base = results["baseline"]["battle_effects"]
            for render in ("beam", "kite"):
                changed = results[f"{render}_zero"]["battle_effects"]
                self.assertGreater(changed[f"max_{render}_width"], 0)
                self.assertLess(changed[f"max_{render}_width"], base[f"max_{render}_width"])
                other = "kite" if render == "beam" else "beam"
                self.assertEqual(changed[f"max_{other}_width"], base[f"max_{other}_width"])
            base = results["baseline"]["live_session"]
            self.assertGreater(base["shield_flash_samples"], 0)
            changed = results["flash_scale"]["live_session"]
            for original, modified in zip(base["max_shield_flash_scale"], changed["max_shield_flash_scale"]):
                self.assertGreater(modified, original)
            self.assertGreater(results["flash_duration"]["live_session"]["shield_flash_samples"],
                               base["shield_flash_samples"])
            self.assertEqual(results["flash_zero"]["live_session"]["shield_flash_samples"], 0)


    def test_live_camera_clip_planes_and_default_distance(self):
        with tempfile.TemporaryDirectory(prefix="eawr-live-camera-depth-") as temporary:
            directory = pathlib.Path(temporary)
            for name, extra, distance in (("default", (), 1200.0),
                                          ("max_out", ("--eawr-camera-zoom", "1"), 1900.0)):
                code, result = self._run(directory, name, (*extra, "--eawr-live-ticks", "5"), camera=None)
                self.assertEqual(code, 0, result.get("failure"))
                camera = result["capture_identity"]["camera"]
                self.assertEqual(camera["near"], 10)
                # Sky geometry widens the render clip; the tactical depth fix is separate.
                self.assertEqual(camera["far"], 60000)
                measured = math.sqrt(sum((eye - target) ** 2 for eye, target in zip(camera["position"], camera["target"])))
                self.assertAlmostEqual(measured, distance, delta=0.05)
