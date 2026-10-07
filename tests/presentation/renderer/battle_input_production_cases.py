"""Cases for test_battle_input; collected by its legacy facade."""

from battle_input_test_support import (
    ACCLAMATOR, ATTACKER_POINT, BattleInputRunner, CAMERA,
    CLEAR_OF_DOGFIGHT, CORUSCANT, CORVETTE, CORVETTE_START,
    DESTINATION, DOGFIGHT_ORDER_TICK, DOGFIGHT_TICK, END_TICK,
    HUD_OFF, LAUNCH_PROBE_TICK, MC80, MC80_ASIDE,
    MEETING, NEBULON, NEBULON_START, OPTIONS_HIT,
    OTHER_TIE_SQUADRON, ROOT, SPOTTER_POINT, SPREAD,
    STAR_BASE, STAR_BASE_POSITION, TARGET_POINT, TARTAN,
    TIE_CRAFT, TIE_SQUADRON, VIEWPORT, X_WING_3_CRAFT,
    X_WING_SQUADRON, X_WING_SQUADRON_3, Y_WINGS, Y_WING_ASIDE,
    Y_WING_SQUADRON, collections, contextlib, decode_png,
    hashlib, inside, json, math,
    os, pathlib, re, read,
    screen, shutil, source_text, strict_json,
    subprocess, sys, tempfile, unittest,
)


class BattleInputProductionCases:
    def test_reinforcement_wheel_facing_persists_and_leaves_camera_still(self):
        import xml.etree.ElementTree as ET
        from tools.inventory.corpus import Corpus
        with tempfile.TemporaryDirectory(prefix="eawr-deploy-facing-") as temporary:
            directory = pathlib.Path(temporary)
            xml = directory / "menu/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text("<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            source = corpus.read_effective("foc", "data/xml/starbases.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            station = next(node for node in tree if node.get("Name") == "Skirmish_Rebel_Star_Base_1")
            for child in list(station):
                if child.tag == "Tactical_Buildable_Objects_Multiplayer": station.remove(child)
            ET.SubElement(station, "Tactical_Buildable_Objects_Multiplayer").text = "Rebel, Corellian_Corvette, Rebel_X-Wing_Squadron"
            (xml / "starbases.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            source = corpus.read_effective("foc", "data/xml/spaceunitscorvettes.xml")
            self.assertIsNotNone(source)
            tree = ET.fromstring(source.data)
            corvette = next(node for node in tree if node.get("Name") == "Corellian_Corvette")
            for tag, value in (("Tactical_Build_Time_Seconds", "1"), ("Tactical_Build_Cost_Multiplayer", "100"),
                               ("Tech_Level", "0")):
                for child in list(corvette):
                    if child.tag == tag: corvette.remove(child)
                ET.SubElement(corvette, tag).text = value
            (xml / "spaceunitscorvettes.xml").write_bytes(ET.tostring(tree, encoding="utf-8"))
            # Four ship drops followed by a separate formation preview.
            common = ["--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off", "--eawr-audio", "off",
                      "--eawr-live-reveal", "on", "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                      "--eawr-live-input", f"10:click:unit={STAR_BASE}"]
            for tick, card in ((20, 0), (25, 0), (30, 0), (35, 0)):
                common += ["--eawr-live-input", f"{tick}:click:card={card}"]
            common += ["--eawr-live-input", "650:click:hud=b_reinforcement"]
            gestures = []
            for index, tick in enumerate((660, 700, 740, 780)):
                place = f"@{STAR_BASE_POSITION[0] + 700 + 300 * index},{STAR_BASE_POSITION[1] - 500},0"
                gestures += ["--eawr-live-input", f"{tick}:press:hud=r_0000",
                             "--eawr-live-input", f"{tick + 5}:hover:{place}"]
                if index in (0, 3): gestures += ["--eawr-live-input", f"{tick + 10}:wheel:{'in' if index == 0 else 'out'}"]
                gestures += ["--eawr-live-input", f"{tick + 15}:release:{place}"]
            # Buy the squadron afterwards so slot zero has one type throughout each sequence.
            squadron = f"@{STAR_BASE_POSITION[0] + 700},{STAR_BASE_POSITION[1] - 900},0"
            gestures += ["--eawr-live-input", "805:click:card=1",
                         "--eawr-live-input", "1300:press:hud=r_0000",
                         "--eawr-live-input", f"1305:hover:{squadron}",
                         "--eawr-live-input", "1315:wheel:in"]
            for label, wheels in (("default", False), ("rotated", True)):
                inputs = gestures if wheels else [item for i, item in enumerate(gestures)
                    if not (item == "--eawr-live-input" and i + 1 < len(gestures) and ":wheel:" in gestures[i + 1]) and ":wheel:" not in item]
                code, result = self._run(directory, label, (*common, *inputs,
                    "--eawr-live-input", "1490:mclick:centre"), end_tick=1500)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertEqual(result["live_session"]["economy_requests"]["reinforcements"], 4, result["battle_input"]["log"])
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                arrivals = [row for row in result["live_session"]["arrivals"] if row["owner"] == 1]
                self.assertEqual(len(arrivals), 4, arrivals)
                arrivals.sort(key=lambda row: row["first_tick"])
                if not wheels:
                    reference = result
                    default_yaw = arrivals[0]["facing_yaw"]
                else:
                    for row in arrivals[:3]:
                        self.assertAlmostEqual((row["facing_yaw"] - default_yaw) % 360, 15, delta=0.01)
                    self.assertAlmostEqual((arrivals[3]["facing_yaw"] - default_yaw) % 360, 0, delta=0.01)
                    # The middle click samples the rendered pose before resetting camera zoom.
                    self.assertEqual(result["battle_input"]["camera_samples"][-2]["eye"],
                                     reference["battle_input"]["camera_samples"][-2]["eye"])
                    samples = [row for row in result["live_session"]["placement_preview"]["samples"] if len(row["clones"]) == 5]
                    self.assertTrue(samples)
                    self.assertGreater(len({row["facing_yaw"] for row in samples}), 1, samples)
                    before, after = samples[0], samples[-1]
                    self.assertAlmostEqual((after["facing_yaw"] - before["facing_yaw"]) % 360, 15, delta=0.01)
                    # Compare craft offsets around the centroid so cursor movement cannot
                    # satisfy the formation rotation contract by translating the squadron.
                    centres = [tuple(sum(point[axis] for point in row["clones"]) / 5
                                     for axis in (0, 1)) for row in (before, after)]
                    angle = math.radians(15)
                    for old, new in zip(before["clones"], after["clones"]):
                        dx, dy = old[0] - centres[0][0], old[1] - centres[0][1]
                        self.assertAlmostEqual(new[0] - centres[1][0], dx * math.cos(angle) - dy * math.sin(angle), delta=0.05)
                        self.assertAlmostEqual(new[1] - centres[1][1], dx * math.sin(angle) + dy * math.cos(angle), delta=0.05)
            # A normal wheel gesture after release still reaches camera zoom.
            code, zoomed = self._run(directory, "zoomed", (*common, *gestures,
                "--eawr-live-input", f"1320:release:{squadron}", "--eawr-live-input", "1400:wheel:out",
                "--eawr-live-input", "1490:mclick:centre"), end_tick=1500)
            self.assertEqual(code, 0, zoomed.get("failure"))
            self.assertNotEqual(zoomed["battle_input"]["camera_samples"][-2]["eye"],
                                result["battle_input"]["camera_samples"][-2]["eye"])

    def test_deployment_overlay_follows_pane(self):
        from PIL import Image
        with tempfile.TemporaryDirectory(prefix="eawr-deploy-overlay-") as temporary:
            directory = pathlib.Path(temporary)
            common = ("--eawr-live-ai", "off", "--eawr-audio", "off",
                      "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                      "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                      "--eawr-live-input", "20:click:card=0",
                      "--eawr-live-input", "500:click:hud=b_reinforcement")
            reports = []
            for label, extra in (("open", ()), ("closed", ("--eawr-live-input", "520:click:hud=r_close"))):
                code, result = self._run(directory, label, (*common, *extra), end_tick=550)
                self.assertEqual(code, 0, result.get("failure"))
                fog = result["live_fog"]
                self.assertEqual(fog["deploy_overlay"], label == "open", fog)
                self.assertEqual(result["hud"]["production"]["pane_open"], label == "open")
                self.assertGreater(fog["prevention_circles"], 0) if label == "open" else None
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                reports.append(result)
            self.assertEqual(reports[0]["live_session"]["final_state_sha256"], reports[1]["live_session"]["final_state_sha256"])
            red_counts = []
            for label in ("open", "closed"):
                with Image.open(directory / f"{label}.png") as picture:
                    # The fog boundary crosses the top strip, above the reinforcement pane.
                    red_counts.append(sum(r > g + 20 and r > b + 20
                        for r, g, b in picture.convert("RGB").crop((0, 0, 1280, 90)).getdata()))
            self.assertGreater(red_counts[0], red_counts[1] + 20, red_counts)

    def test_deployment_overlay_enemy_circle_rejects_drop(self):
        from live_session_test_support import EMPIRE_STATION
        with tempfile.TemporaryDirectory(prefix="eawr-deploy-circle-") as temporary:
            directory = pathlib.Path(temporary)
            code, layout = self._run(directory, "layout", ("--eawr-live-ai", "off",), end_tick=3)
            self.assertEqual(code, 0, layout.get("failure"))
            station = next(row for row in layout["live_session"]["start_markers"]
                           if row["player"] == 2 and row["use"] == "station")
            x, y, _ = station["position"]
            camera = directory / "circle-camera.xml"
            camera.write_text(re.sub(r"<initial [^>]*/>",
                f'<initial target_x="{x - 1800}" target_y="{y}" target_height="0" zoom="0.5" yaw_degrees="0"/>',
                CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
            shutil.copyfile(CAMERA.parent / "space-live-camera-bindings.json", directory / "space-live-camera-bindings.json")
            orders = [argument for hardpoint in range(2, 7)
                      for argument in ("--eawr-live-order", f"1:damage:{EMPIRE_STATION}@100000,{hardpoint}")]
            for enemy in (11, 12):
                orders += ["--eawr-live-order", f"1:damage:{enemy}@1000000"]
            # One scout reveals both sides of the 2000-unit boundary while outside weapon reach.
            orders += ["--eawr-live-order", f"15:move:{CORVETTE}@{x - 2400},{y},0"]
            code, result = self._run(directory, "circle", (*orders,
                "--eawr-live-ai", "off", "--eawr-live-step", "10", "--eawr-audio", "off",
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                "--eawr-live-input", f"10:click:unit={STAR_BASE}", "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "3400:click:hud=b_reinforcement",
                "--eawr-live-input", "3500:press:hud=r_0000",
                "--eawr-live-input", f"3520:hover:@{x - 1800},{y},0",
                "--eawr-live-input", f"3540:release:@{x - 1800},{y},0",
                "--eawr-live-input", "3580:press:hud=r_0000",
                "--eawr-live-input", f"3600:hover:@{x - 2700},{y},0",
                "--eawr-live-input", f"3620:release:@{x - 2700},{y},0"), camera=camera, end_tick=3850)
            self.assertEqual(code, 0, result.get("failure"))
            requests = result["live_session"]["economy_requests"]
            self.assertFalse(any("not on screen" in entry for entry in result["battle_input"]["log"]))
            self.assertEqual((requests["refused"], requests["reinforcements"]), (1, 1), requests)
            fog = result["live_fog"]
            self.assertTrue(fog["deploy_overlay"], fog)
            self.assertTrue(any(row["blocked_points"] > 0 for row in fog["overlay_samples"]), fog)
            self.assertTrue(result["live_session"]["headless_hashes_equal"])

    def test_reinforcement_slots_ignore_completion_order(self):
        # WR-08/EUS-14: compare grouped slots within one definition set, without
        # claiming any particular stock ordering across loads.
        with tempfile.TemporaryDirectory(prefix="eawr-pool-order-") as temporary:
            directory = pathlib.Path(temporary)
            panels = []
            for label, first, second in (("a_b", 0, 1), ("b_a", 1, 0)):
                code, result = self._run(directory, label, (
                    "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                    "--eawr-live-input", f"20:click:card={first}",
                    "--eawr-live-input", f"25:click:card={second}",
                    "--eawr-live-input", "1100:click:hud=b_reinforcement"), end_tick=1150)
                self.assertEqual(code, 0, result.get("failure"))
                self.assertEqual(result["live_session"]["economy_requests"]["buys"], 2)
                panel = result["hud"]["production"]
                self.assertTrue(panel["pane_open"], panel)
                self.assertEqual(len(panel["pool"]), 2, panel)
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                panels.append([(slot["slot"], slot["type"], slot["text"], slot["enabled"])
                               for slot in panel["pool"]])
            self.assertEqual(panels[0], panels[1])

    def test_second_teammate_buys_and_deploys_from_shared_station(self):
        # WPR-11/30/32/33: the local second teammate uses its own credits, queue and pool.
        place = (STAR_BASE_POSITION[0] + 700.0, STAR_BASE_POSITION[1] - 500.0)
        with tempfile.TemporaryDirectory(prefix="eawr-team-purchase-") as temporary:
            directory = pathlib.Path(temporary)
            camera = directory / "station-camera.xml"
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            camera.write_text(CAMERA.read_text(encoding="utf-8").replace(
                'target_x="-4850" target_y="4400"',
                f'target_x="{STAR_BASE_POSITION[0] + 350.0}" target_y="{STAR_BASE_POSITION[1] - 250.0}"'),
                encoding="utf-8")
            extra = ["--eawr-skirmish-players", "1,2,3,4",
                     "--eawr-live-ai", "off", "--eawr-live-player", "3",
                     "--eawr-live-reveal", "on", "--eawr-environment", "map",
                     "--eawr-lighting", "sh", "--eawr-shadows", "on",
                     "--eawr-skirmish-slot", "1:Rebel:0:ai", "--eawr-skirmish-slot", "2:Empire:1:ai",
                     "--eawr-skirmish-slot", "3:Rebel:0:human", "--eawr-skirmish-slot", "4:Empire:1:ai",
                     "--eawr-live-input", "10:click:unit=1", "--eawr-live-input", "20:click:card=0",
                     "--eawr-live-input", "500:click:hud=b_reinforcement",
                     "--eawr-live-input", "510:press:hud=r_0000",
                     "--eawr-live-input", f"515:hover:@{place[0]},{place[1]},0",
                     "--eawr-live-input", f"520:release:@{place[0]},{place[1]},0",
                     "--eawr-live-capture-ticks", "30,480,700"]
            code, result = self._run(directory, "team-purchase", extra, camera=camera,
                                     end_tick=720, session="skirmish")
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["production"]["station"], 1, battle["log"])
            self.assertEqual(battle["production"]["buys"], 1, battle["log"])
            self.assertEqual(battle["production"]["placements"], 1, battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertEqual(live["economy"]["pool"], [])
            self.assertEqual(live["economy"]["population"], 1)
            self.assertLessEqual(abs(int(live["economy"]["credits"]) - (6000 - 500 + 720 * 5 // 30)), 2)
            arrivals = [row for row in live["arrivals"] if row["owner"] == 3]
            self.assertGreaterEqual(len(arrivals), 2, live["arrivals"])
            self.assertTrue(all(row["owner"] == 3 for row in live["arrivals"]), live["arrivals"])

    def test_build_limits_reserve_hide_and_cancel(self):
        # WPR-60..62: observe actual drawn cards through the station's production UI.
        research = "RS_Enhanced_Shielding_L1_Upgrade"
        original = ["Rebel_X-Wing_Squadron", "Y-Wing_Squadron", research,
                    "RS_Improved_Weapons_L1_Upgrade", "RS_Level_Two_Starbase_Upgrade"]
        for name, end_tick, cancel in (("reserved", 100, False), ("cancelled", 140, True),
                                       ("researched", 1000, False)):
            with self.subTest(state=name), tempfile.TemporaryDirectory(prefix="eawr-menu-limit-") as temporary:
                extra = ["--eawr-live-ai", "off", "--eawr-environment", "map",
                         "--eawr-lighting", "sh", "--eawr-shadows", "on",
                         "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                         "--eawr-live-input", "20:click:card=2"]
                if cancel: extra += ["--eawr-live-input", "100:rclick:hud=tqueue00"]
                code, result = self._run(pathlib.Path(temporary), name, extra, end_tick=end_tick)
                self.assertEqual(code, 0, result.get("failure"))
                cards = sorted(result["hud"]["unit_cards"]["drawn"], key=lambda card: card["slot"])
                self.assertEqual([card["type"] for card in cards],
                                 [item for item in original if name != "researched" or item != research], cards)
                if name != "researched":
                    self.assertEqual(cards[2]["disabled"], name == "reserved", cards)
                self.assertEqual([card["slot"] for card in cards], list(range(len(cards))), cards)
                self.assertEqual(result["hud"]["production"]["cancels"], int(cancel))
                self.assertEqual(result["live_session"]["economy_requests"]["cancels"], int(cancel))
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                self.assertEqual(result["live_session"]["rejected"], [])

    def test_station_build_buttons_show_the_menu(self):
        # #530 PU-60 to PU-62: the Rebel level-1 station's menu in list order, each button with its
        # Icon_Name drawn and its multiplayer price (the retail still shows 500, 550, 850, 800 and
        # 2000). WPR-33/50: the upgrades and level-up are available before any is queued.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-menu-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "menu", ("--eawr-live-input", f"10:click:unit={STAR_BASE}"), end_tick=30)
            self.assertEqual(code, 0, result.get("failure"))
            drawn = sorted(result["hud"]["unit_cards"]["drawn"], key=lambda card: card["slot"])
            self.assertEqual([card["type"] for card in drawn],
                             ["Rebel_X-Wing_Squadron", "Y-Wing_Squadron", "RS_Enhanced_Shielding_L1_Upgrade",
                              "RS_Improved_Weapons_L1_Upgrade", "RS_Level_Two_Starbase_Upgrade"], drawn)
            self.assertEqual([card["price"] for card in drawn], [500, 550, 850, 800, 2000], drawn)
            self.assertEqual([card["build_frames"] for card in drawn], [450, 510, 900, 750, 2400], drawn)
            self.assertEqual([card["disabled"] for card in drawn], [False] * 5, drawn)
            self.assertTrue(all(card["icon_drawn"] for card in drawn), drawn)


    def test_station_levelup_retains_control_group(self):
        # WPR-52/WP-23: recall the group after replacing its station, through Godot input.
        # The two factions exercise different station/menu chains; selection is cleared before recall.
        for faction in ("Rebel", "Empire"):
            with self.subTest(faction=faction), tempfile.TemporaryDirectory(prefix="eawr-station-group-") as temporary:
                extra = ["--eawr-live-ai", "off", "--eawr-live-player", "1", "--eawr-live-follow", "1",
                         "--eawr-live-reveal", "on", "--eawr-live-step", "1", "--eawr-map-timed-frames", "2640",
                         "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                         "--eawr-live-workers", "2", "--eawr-live-capture-ticks", "1,2500,2640"]
                if faction == "Empire":
                    extra += ["--eawr-skirmish-slot", "1:Empire:0:human", "--eawr-skirmish-slot", "2:Rebel:1:ai",
                              "--eawr-skirmish-fleet", "1:none", "--eawr-skirmish-fleet", "2:none"]
                inputs = ["1:click:unit=1", "15:key:1+ctrl", "30:click:card=4", "2590:key:2", "2600:key:1"]
                if faction == "Rebel": inputs.insert(1, "10:click:card=2")
                for gesture in inputs: extra += ["--eawr-live-input", gesture]
                code, result = self._run(pathlib.Path(temporary), faction.lower(), extra, end_tick=2640,
                    camera=CAMERA if faction == "Rebel" else None,
                    map_path=CORUSCANT if faction == "Rebel" else "data/art/maps/_mp_space_polus.ted",
                    session="m2" if faction == "Rebel" else "skirmish")
                self.assertEqual(code, 0, result.get("failure"))
                battle, live = result["battle_input"], result["live_session"]
                self.assertEqual(live["economy"]["tech_level"], 2, live["economy"])
                self.assertIs(live["headless_hashes_equal"], True)
                self.assertEqual(live["rejected"], [])
                self.assertEqual(battle["production"]["buys"], 2 if faction == "Rebel" else 1, battle["log"])
                selected = battle["selected"]
                self.assertEqual(len(selected), 1, battle["log"])
                self.assertNotEqual(selected, [1], battle["log"])
                self.assertEqual(battle["groups"].get("1"), selected, battle["log"])
                self.assertEqual(battle["production"]["station"], selected[0], battle["production"])
                self.assertIn(selected[0], [unit["entity"] for unit in live["own_units"]])
                self.assertTrue(result["hud"]["unit_cards"]["drawn"], result["hud"])


    def test_paused_drop_leaves_the_reserve_pane(self):
        # TM-10: a drop made while paused waits for the next tick. Until that tick runs, the pane
        # leaves the dropped unit out, so a second press on its slot finds it empty: the squadron
        # is sent once and nothing is rejected on resume (the second drop used to reach the
        # simulation as not_in_pool, and units arrived where the first drop pointed).
        first = (STAR_BASE_POSITION[0] + 700.0, STAR_BASE_POSITION[1] - 500.0)
        second = (STAR_BASE_POSITION[0] + 1000.0, STAR_BASE_POSITION[1] + 440.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-paused-drop-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "paused_drop", (
                "--eawr-live-step", "10",
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:click:hud=pause",
                "--eawr-live-input", "f80:press:hud=r_0000",
                "--eawr-live-input", f"f85:hover:@{first[0]},{first[1]},0",
                "--eawr-live-input", f"f90:release:@{first[0]},{first[1]},0",
                "--eawr-live-input", "f100:press:hud=r_0000",
                "--eawr-live-input", f"f105:hover:@{second[0]},{second[1]},0",
                "--eawr-live-input", f"f110:release:@{second[0]},{second[1]},0",
                "--eawr-live-input", "f120:click:hud=resume"), end_tick=720)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 11, battle["log"])
            self.assertEqual(battle["production"]["placements"], 1, battle["log"])
            # The second press found the slot empty: one placement began.
            self.assertEqual(sum(row.startswith("reinforce placing") for row in battle["log"]), 1, battle["log"])
            requests = live["economy_requests"]
            self.assertEqual((requests["buys"], requests["reinforcements"], requests["refused"]), (1, 1, 0), requests)
            self.assertEqual(live["rejected"], [])
            arrivals = [row for row in live["arrivals"] if row["owner"] == 1]
            self.assertGreaterEqual(len(arrivals), 2, live["arrivals"])
            self.assertEqual(live["economy"]["pool"], [], live["economy"])
            self.assertIs(live["headless_hashes_equal"], True)

    def test_station_buys_a_squadron_that_arrives(self):
        # #530 (docs/behaviour/space-purchasing.md PU-60 to PU-68): selecting the Rebel station turns
        # the card slots into its build buttons; the first buys an X-wing squadron (500 credits, 450
        # frames). Once it is pooled, the reinforcements button opens the pane, a press on its slot
        # starts a drag; its release on the battle plane brings it in through hyperspace: hidden
        # until its frame 35, landed at frame 150. Every step is a player gesture through the HUD.
        place = (STAR_BASE_POSITION[0] + 700.0, STAR_BASE_POSITION[1] - 500.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-purchase-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "purchase", (
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:press:hud=r_0000",
                "--eawr-live-input", f"515:hover:@{place[0]},{place[1]},0",
                "--eawr-live-input", f"520:release:@{place[0]},{place[1]},0",
                "--eawr-live-capture-ticks", "30,480,512,540,560,700"), end_tick=720)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live, hud = result["battle_input"], result["live_session"], result["hud"]
            self.assertEqual(battle["scripted_fired"], 6, battle["log"])
            production = battle["production"]
            self.assertEqual(production["station"], STAR_BASE, battle["log"])
            self.assertEqual(production["buys"], 1, battle["log"])
            self.assertEqual(production["placements"], 1, battle["log"])
            buttons = production["buttons"]
            self.assertGreaterEqual(len(buttons), 2, buttons)
            self.assertEqual(buttons[0]["price"], 500, buttons)
            requests = live["economy_requests"]
            self.assertEqual((requests["buys"], requests["reinforcements"], requests["refused"]), (1, 1, 0), requests)
            self.assertEqual(live["rejected"], [])
            # The squadron's craft and container arrived: hidden for 35 frames, landed after 150.
            preview = live["placement_preview"]
            self.assertGreater(preview["frames"], 0, preview)
            valid = [row for row in preview["samples"] if row["valid"]]
            self.assertTrue(valid, preview)
            self.assertTrue(all(len(row["clones"]) == 5 for row in valid), valid)
            self.assertGreater(len({tuple(pose) for pose in valid[-1]["clones"]}), 1, valid[-1])
            arrivals = [row for row in live["arrivals"] if row["owner"] == 1]
            self.assertGreaterEqual(len(arrivals), 2, live["arrivals"])
            # Scripted input enters Godot's queue; compare the preview with the observed
            # command application rather than its requested input tick.
            first_arrival = min(row["first_tick"] for row in arrivals)
            self.assertTrue(all(row["tick"] < first_arrival for row in preview["samples"]), preview)
            self.assertIsNone(production["placing"], production)
            for row in arrivals:
                self.assertEqual(row["visible_tick"] - row["first_tick"], 35, row)
                self.assertEqual(row["landed_tick"] - row["first_tick"], 150, row)
            # Drawn once landed: the squadron's five craft have models (PU-G25's purchase slots).
            drawn = {unit["entity"] for unit in live["own_units"]}
            self.assertGreaterEqual(len(drawn & {row["unit"] for row in arrivals}), 5, live["own_units"])
            economy = live["economy"]
            self.assertEqual(economy["pool"], [], economy)
            self.assertEqual(economy["population"], 1, economy)
            # 6000 - 500 + about 720 frames of 5 credits a second (PU-07: Q24 per-frame steps, the HUD
            # rounds down, and the last presented frame may trail the session by a tick).
            self.assertLessEqual(abs(int(economy["credits"]) - (6000 - 500 + 720 * 5 // 30)), 2, economy)
            self.assertIs(live["headless_hashes_equal"], True)
            panel = hud["production"]
            self.assertTrue(panel["pane_open"], panel)
            self.assertEqual(panel["picks"], 1, panel)
            self.assertEqual(panel["queue_slots"], 10, panel)
            self.assertEqual(panel["pane_slots"], 20, panel)
            self.assertTrue(panel["reinforce_drawn"], panel)
            self.assertEqual(panel["flashes_started"], 1, panel)
            self.assertGreater(panel["flash_frames"], 0, panel)
            self.assertFalse(panel["flash_drawn"], panel)  # Opening the pane stops its notification.


    def test_reinforcement_close_button_matches_its_visible_art(self):
        # PU-67, PU-72: the one-row reserve uses its row alternate, the localized
        # close label and the same rectangle for the art and mouse input.
        with tempfile.TemporaryDirectory(prefix="eawr-reinforcement-close-") as temporary:
            directory = pathlib.Path(temporary)
            inputs = ("--eawr-live-input", f"10:click:unit={STAR_BASE}",
                      "--eawr-live-input", "20:click:card=0",
                      "--eawr-live-input", "25:click:card=0",
                      "--eawr-live-input", "1000:click:hud=b_reinforcement")
            code, opened = self._run(directory, "opened", inputs, end_tick=1050)
            self.assertEqual(code, 0, opened.get("failure"))
            panel = opened["hud"]["production"]
            self.assertTrue(panel["pane_open"], panel)
            self.assertEqual(panel["pool"][0]["text"], "x2", panel)
            self.assertEqual(panel["pane_rows"], 1, panel)
            self.assertEqual(panel["pane_meshes"], ["reinforcement_window_ALT0"], panel)
            self.assertEqual(panel["close_text_key"], "TEXT_BUTTON_CLOSE", panel)
            self.assertEqual(panel["close_label"], "Close", panel)
            self.assertEqual(panel["close_rect"], panel["close_art_rect"], panel)
            x, y, _, height = panel["close_art_rect"]
            # The visible left edge extends beyond the old shell mesh hit target.
            point = f"screen={x + 2},{y + height / 2}"
            code, closed = self._run(directory, "closed", (*inputs,
                "--eawr-live-input", f"1020:click:{point}"), end_tick=1050)
            self.assertEqual(code, 0, closed.get("failure"))
            self.assertFalse(closed["hud"]["production"]["pane_open"], closed["hud"]["production"])
            self.assertEqual(closed["live_session"]["economy"]["pool"],
                             opened["live_session"]["economy"]["pool"])
            self.assertTrue(closed["live_session"]["headless_hashes_equal"])


    def test_warmed_production_art_allocates_nothing(self):
        # Count extension-owned allocations in the actual button/dial redraw paths,
        # and geometry placements separately (Godot rendering internals are excluded).
        from unittest.mock import patch
        with patch.dict(os.environ, {"EAWR_HUD_ALLOCATION_TEST": "1"}), tempfile.TemporaryDirectory(prefix="eawr-hud-allocation-") as temporary:
            directory = pathlib.Path(temporary)
            states = (
                ("empty", 60, ()),
                ("queued", 150, ("--eawr-live-input", f"10:click:unit={STAR_BASE}",
                                 "--eawr-live-input", "20:click:card=0")),
                ("flashing", 600, ("--eawr-live-input", f"10:click:unit={STAR_BASE}",
                                   "--eawr-live-input", "20:click:card=0")))
            for name, tick, gestures in states:
                with self.subTest(state=name):
                    code, result = self._run(directory, name, ("--eawr-live-ai", "off", *gestures), end_tick=tick)
                    self.assertEqual(code, 0, result.get("failure"))
                    panel = result["hud"]["production"]
                    self.assertGreater(panel["warmed_art_frames"], 10, panel)
                    self.assertEqual(panel["art_allocations"], 0, panel)
                    self.assertEqual(panel["art_buffer_work"], 0, panel)
                    self.assertLessEqual(panel["art_builds"], 2, panel)
                    if name == "queued":
                        self.assertEqual(panel["dials_drawn"], 1, panel)
                    if name == "flashing":
                        self.assertGreater(panel["flash_frames"], 10, panel)
                    self.assertTrue(result["live_session"]["headless_hashes_equal"])


    def test_reinforcement_button_draws_before_hover_and_flashes(self):
        # PU-70/71: draw evidence comes from _draw(), before any gesture near the button.
        with tempfile.TemporaryDirectory(prefix="eawr-reinforce-visible-") as temporary:
            directory = pathlib.Path(temporary)
            code, idle = self._run(directory, "idle", end_tick=2)
            self.assertEqual(code, 0, idle.get("failure"))
            button = idle["hud"]["production"]
            self.assertTrue(button["reinforce_drawn"], button)
            self.assertFalse(button["reinforce_enabled"], button)
            self.assertEqual(button["toggles"], 0, button)
            code, completed = self._run(directory, "completed", (
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0"), end_tick=480)
            self.assertEqual(code, 0, completed.get("failure"))
            button = completed["hud"]["production"]
            self.assertTrue(button["reinforce_drawn"], button)
            self.assertTrue(button["reinforce_enabled"], button)
            self.assertEqual(button["toggles"], 0, button)
            self.assertEqual(button["flashes_started"], 1, button)
            self.assertTrue(button["flash_continuous"], button)
            self.assertEqual(button["flash_period_seconds"], 0.5, button)
            self.assertTrue(button["flash_drawn"], button)
            self.assertGreater(button["flash_frames"], 0, button)
            self.assertTrue(completed["live_session"]["headless_hashes_equal"])
            code, finished = self._run(directory, "finished", (
                "--eawr-live-ai", "off",
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0"), end_tick=1400)
            self.assertEqual(code, 0, finished.get("failure"))
            button = finished["hud"]["production"]
            self.assertTrue(button["reinforce_drawn"], button)
            self.assertTrue(button["flash_drawn"], button)  # Still pulses more than 30 s after completion.
            self.assertGreater(button["flash_level"], 0, button)


    def test_queue_dial_grows_and_only_right_click_cancels(self):
        # PU-63/64, #982: compare actual dial pixels at about 10% and 90%; left click
        # keeps both entries and right click refunds the selected front entry.
        from PIL import Image
        with tempfile.TemporaryDirectory(prefix="eawr-queue-sweep-") as temporary:
            directory = pathlib.Path(temporary)
            gestures = ("--eawr-live-input", f"10:click:unit={STAR_BASE}",
                        "--eawr-live-input", "20:click:card=0",
                        "--eawr-live-input", "25:click:card=0",
                        "--eawr-live-input", "100:click:hud=tqueue05")
            code, kept = self._run(directory, "sweep", (*gestures,
                "--eawr-live-capture-ticks", "30,70,430"), end_tick=430)
            self.assertEqual(code, 0, kept.get("failure"))
            panel = kept["hud"]["production"]
            self.assertEqual(panel["cancels"], 0, panel)
            self.assertEqual(len(panel["queue"]), 2, panel)
            self.assertEqual(panel["dials_drawn"], 1, panel)
            front = panel["queue"][0]
            self.assertGreater(front["progress"], 0.85, front)
            x, y, width, height = front["rect"]
            crop = (round(x - 5), round(y - 5), round(x + width + 5), round(y + height + 5))
            images = []
            for tick in (30, 70, 430):
                with Image.open(directory / f"sweep_t{tick:04d}.png") as image:
                    images.append(list(image.convert("RGB").crop(crop).getdata()))
            def changed(pixels):
                return sum(max(abs(a - b) for a, b in zip(pixel, base)) > 20
                           for pixel, base in zip(pixels, images[0]))
            early, late = changed(images[1]), changed(images[2])
            self.assertGreater(late, max(early * 2, 20), (early, late, front))
            code, canceled = self._run(directory, "cancel", (*gestures,
                "--eawr-live-input", "435:rclick:hud=tqueue05"), end_tick=445)
            self.assertEqual(code, 0, canceled.get("failure"))
            panel = canceled["hud"]["production"]
            self.assertEqual(panel["cancels"], 1, panel)
            self.assertEqual(len(panel["queue"]), 1, panel)
            self.assertEqual(canceled["live_session"]["economy_requests"]["cancels"], 1)
            self.assertGreaterEqual(int(panel["credits"]), int(kept["hud"]["production"]["credits"]) + 500)
            self.assertTrue(canceled["live_session"]["headless_hashes_equal"])


    def test_invalid_reinforcement_drag_cancels_and_keeps_the_pool(self):
        # WR-13/15: the occupied station's point is red, release cancels, and a later click cannot deploy.
        blocked = f"@{STAR_BASE_POSITION[0]},{STAR_BASE_POSITION[1]},0"
        with tempfile.TemporaryDirectory(prefix="eawr-invalid-arrival-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "invalid", (
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:press:hud=r_0000",
                "--eawr-live-input", f"515:hover:{blocked}",
                "--eawr-live-input", f"520:release:{blocked}",
                "--eawr-live-input", f"530:click:{blocked}"), end_tick=540)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["economy_requests"]["reinforcements"], 0, live)
            self.assertEqual(live["economy_requests"]["refused"], 1, live)
            self.assertEqual(len(live["economy"]["pool"]), 1, live)
            self.assertEqual(live["arrivals"], [], live)
            samples = live["placement_preview"]["samples"]
            self.assertTrue(samples, live["placement_preview"])
            self.assertTrue(any(not row["valid"] for row in samples), samples)
            self.assertTrue(all(row["tick"] < 530 for row in samples), samples)
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], 7, battle["log"])
            self.assertIsNone(battle["production"]["placing"], battle["production"])


    def test_pending_victory_closes_the_pane_and_cancels_a_drag(self):
        # WR-07/19: both an already selected reserve and a new pane request are gated.
        with tempfile.TemporaryDirectory(prefix="eawr-arrival-victory-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "arrival_victory", (
                "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                "--eawr-live-input", "20:click:card=0",
                "--eawr-live-input", "500:click:hud=b_reinforcement",
                "--eawr-live-input", "510:press:hud=r_0000",
                "--eawr-live-order", "515:damage:8@1000000",
                "--eawr-live-input", "520:release:@-3111,3960,0",
                "--eawr-live-input", "530:click:hud=b_reinforcement"), end_tick=540)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIsNotNone(live["outcome"], live)
            self.assertEqual(live["economy_requests"]["reinforcements"], 0, live)
            self.assertEqual(len(live["economy"]["pool"]), 1, live)
            self.assertEqual(live["arrivals"], [], live)
            self.assertFalse(result["hud"]["production"]["pane_open"], result["hud"])
            self.assertIsNone(result["battle_input"]["production"]["placing"], result["battle_input"])


    def test_bought_units_reuse_the_slots_of_the_dead(self):
        # #530 PU-G25: the purchase slots are what can stand at once, not how many were ever
        # bought. With one slot per buyable craft (--eawr-live-purchase-slots 1) the first bought
        # X-wing squadron takes every X-wing slot; its craft die (scripted damage as their owner)
        # and the second squadron, bought meanwhile and brought in after that, is drawn on the same
        # slots. Before the fix the second squadron was simulated but never drawn. A first run
        # without the kills finds the craft's entity IDs (the AI's launches decide them; the
        # simulation is deterministic, so the second run sees the same ones).
        place = (STAR_BASE_POSITION[0] + 700.0, STAR_BASE_POSITION[1] - 500.0)
        second_place = (place[0] + 300.0, place[1])
        second_arrives = 1000
        with tempfile.TemporaryDirectory(prefix="eawr-battle-slot-reuse-") as temporary:
            directory = pathlib.Path(temporary)

            def run(name, kills=()):
                return self._run(directory, name, (
                    "--eawr-live-purchase-slots", "1", "--eawr-live-late-orders", "on",
                    "--eawr-live-input", f"10:click:unit={STAR_BASE}",
                    "--eawr-live-input", "20:click:card=0",
                    "--eawr-live-input", "30:click:card=0",
                    "--eawr-live-input", "500:click:hud=b_reinforcement",
                    "--eawr-live-input", "510:press:hud=r_0000",
                    "--eawr-live-input", f"515:hover:@{place[0]},{place[1]},0",
                    "--eawr-live-input", f"520:release:@{place[0]},{place[1]},0",
                    *(argument for craft in kills
                      for argument in ("--eawr-live-order", f"800:damage:{craft}@100000")),
                    "--eawr-live-input", f"{second_arrives}:press:hud=r_0000",
                    "--eawr-live-input", f"{second_arrives + 5}:hover:@{second_place[0]},{second_place[1]},0",
                    "--eawr-live-input", f"{second_arrives + 10}:release:@{second_place[0]},{second_place[1]},0",
                    "--eawr-live-capture-ticks", "700,1300"), end_tick=1320)

            code, probe = run("slot-probe")
            self.assertEqual(code, 0, probe.get("failure"))
            rows = [row for row in probe["live_session"]["arrivals"] if row["owner"] == 1]
            craft_type = collections.Counter(row["type"] for row in rows).most_common(1)[0][0]
            craft = [row for row in rows if row["type"] == craft_type]
            first_craft = sorted(row["unit"] for row in craft if row["first_tick"] < 800)
            second_craft = sorted(row["unit"] for row in craft if row["first_tick"] >= 800)
            self.assertEqual((len(first_craft), len(second_craft)), (5, 5), rows)
            # Without the kills the second squadron has no free slot.
            drawn = {unit["entity"] for unit in probe["live_session"]["own_units"]}
            self.assertEqual(drawn & set(second_craft), set(), sorted(drawn))

            code, result = run("slot-reuse", first_craft)
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [], live["rejected"])
            self.assertEqual(live["economy_requests"]["buys"], 2, live["economy_requests"])
            self.assertEqual(live["economy_requests"]["reinforcements"], 2, live["economy_requests"])
            arrivals = {row["unit"] for row in live["arrivals"] if row["owner"] == 1}
            self.assertTrue(set(first_craft) | set(second_craft) <= arrivals, live["arrivals"])
            drawn = {unit["entity"] for unit in live["own_units"]}
            self.assertEqual(drawn & set(first_craft), set(), "the first squadron died")
            self.assertTrue(set(second_craft) <= drawn, (sorted(drawn), live["arrivals"]))
            self.assertGreaterEqual(live["slots_released"], len(first_craft), live)
            self.assertIs(live["headless_hashes_equal"], True)
