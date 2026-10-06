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
