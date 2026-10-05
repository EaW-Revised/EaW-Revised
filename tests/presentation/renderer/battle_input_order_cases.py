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


class BattleInputOrderCases:
    def test_box_skips_station_and_keeps_ship_cards(self):
        # WSU-21..24: exercise real Godot pointer input with the station and fleet both in view.
        captures = os.environ.get("EAWR_BOX_SELECTION_CAPTURE_DIR")
        context = (contextlib.nullcontext(captures) if captures
                   else tempfile.TemporaryDirectory(prefix="eawr-box-selection-"))
        with context as output:
            directory = pathlib.Path(output)
            directory.mkdir(parents=True, exist_ok=True)
            camera = directory / "camera.xml"
            camera.write_text(CAMERA.read_text(encoding="utf-8")
                .replace('target_x="-4850" target_y="4400"', 'target_x="-4400" target_y="4150"')
                .replace('zoom="0.611111"', 'zoom="0.92"')
                .replace('path="space-live-camera-bindings.json"',
                         f'path="{(CAMERA.parent / "space-live-camera-bindings.json").as_posix()}"'), encoding="utf-8")
            look = ("--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                    "--eawr-live-ai", "off", "--eawr-live-workers", "4")
            code, station = self._run(directory, "station", (*look,
                "--eawr-live-input", "10:click:unit=1"), camera=camera, end_tick=60)
            self.assertEqual(code, 0, station.get("failure"))
            self.assertEqual(station["battle_input"]["selected"], [STAR_BASE])
            self.assertEqual(station["battle_input"]["production"]["station"], STAR_BASE)
            gestures = ("--eawr-live-input", "10:click:unit=1",
                        "--eawr-live-input", "20:box:screen=30,30/screen=1250,520")
            code, ships = self._run(directory, "ships", (*look, *gestures), camera=camera, end_tick=60)
            self.assertEqual(code, 0, ships.get("failure"))
            battle = ships["battle_input"]
            self.assertEqual(battle["boxes"], 1)
            self.assertNotIn(STAR_BASE, battle["selected"], battle["log"])
            self.assertIn(NEBULON, battle["selected"], battle["log"])
            self.assertIn(MC80, battle["selected"], battle["log"])
            self.assertIsNone(battle["production"]["station"])
            self.assertTrue(battle["unit_cards"]["cards"])
            self.assertTrue(ships["hud"]["unit_cards"]["drawn"])
            self.assertEqual(ships["live_session"]["final_state_sha256"], station["live_session"]["final_state_sha256"])
            # Keep the newly launched craft northeast of the station outside this origin-only box.
            station_box = "40:box:@-3961,4610,0/@-3661,4310,0"
            code, kept = self._run(directory, "station-only", (*look, *gestures,
                "--eawr-live-input", station_box), camera=camera, end_tick=60)
            self.assertEqual(code, 0, kept.get("failure"))
            self.assertEqual(kept["battle_input"]["boxes"], 2)
            self.assertEqual(kept["battle_input"]["selected"], battle["selected"])
            code, mixed = self._run(directory, "mixed", (*look, *gestures,
                "--eawr-live-input", "40:click:unit=1+shift"), camera=camera, end_tick=60)
            self.assertEqual(code, 0, mixed.get("failure"))
            self.assertIn(STAR_BASE, mixed["battle_input"]["selected"])
            self.assertIn(NEBULON, mixed["battle_input"]["selected"])
            self.assertEqual(mixed["battle_input"]["production"]["station"], STAR_BASE)
            for result in (ships, kept, mixed):
                self.assertTrue(result["live_session"]["headless_hashes_equal"])
                self.assertEqual(result["live_session"]["final_state_sha256"], station["live_session"]["final_state_sha256"])


    def test_click_selects_and_right_click_moves(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-move-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "move", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0",
                "--eawr-live-capture-ticks", "10,20,80,150"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            # The M2 start stacks the Rebel companies on their spawn: the click at the corvette's
            # centre picks the highest contact there (P-1), one unit.
            self.assertEqual(len(battle["selected"]), 1, battle["log"])
            picked = battle["selected"][0]
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            order = next(line for line in battle["log"] if line.startswith("move @"))
            match = re.fullmatch(r"move @(\S+) units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            point, units, tick = match.group(1), int(match.group(2)), int(match.group(3))
            self.assertEqual(units, picked)
            # The right click lands where the pointer was: the battle-plane point of the projected
            # destination.
            x, y, _ = (float(value) for value in point.split(","))
            self.assertLess(math.dist((x, y), DESTINATION), 1.0, point)
            # The picked unit left the stack toward it.
            position = self._position(result, picked)
            self.assertGreater(math.dist(CORVETTE_START, position[:2]), 100.0, position)
            self.assertLess(math.dist(DESTINATION, position[:2]), math.dist(DESTINATION, CORVETTE_START), position)
            # The same order through the debug hook at the tick the click was stamped for gives the
            # same unit the same path: the click is an ordinary recorded command.
            code, hook = self._run(directory, "hook", (*HUD_OFF, "--eawr-live-order", f"{tick}:move:{picked}@{point}"))
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(self._position(hook, picked), position)
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])



    # #452 (docs/behaviour/space-orders.md OR-01): Ctrl+right click on empty space attack-moves the
    # selection, exactly as the same order through the debug hook.
    def test_ctrl_right_click_attack_moves(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-move-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "attack_move", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0+ctrl"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (2, 1), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            picked = battle["selected"][0]
            order = next(line for line in battle["log"] if line.startswith("attack-move @"))
            match = re.fullmatch(r"attack-move @(\S+) units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            point, units, tick = match.group(1), int(match.group(2)), int(match.group(3))
            self.assertEqual(units, picked)
            x, y, _ = (float(value) for value in point.split(","))
            self.assertLess(math.dist((x, y), DESTINATION), 1.0, point)
            position = self._position(result, picked)
            self.assertLess(math.dist(DESTINATION, position[:2]), math.dist(DESTINATION, CORVETTE_START), position)
            code, hook = self._run(directory, "attack_move_hook",
                                   (*HUD_OFF, "--eawr-live-order", f"{tick}:attack_move:{picked}@{point}"))
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])



    # OR-01, OR-14: Ctrl+Alt+right click on an own unit outside the selection (the Rebel star base)
    # guards it: the ship closes to within the guard range, as the same order through the hook.
    def test_ctrl_alt_right_click_guards_an_own_unit(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-guard-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "guard", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"15:rclick:unit={STAR_BASE}+ctrl+alt"), end_tick=450)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (2, 1), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            picked = battle["selected"][0]
            order = next(line for line in battle["log"] if line.startswith("guard "))
            match = re.fullmatch(rf"guard {STAR_BASE} units (\d+) tick (\d+)", order)
            self.assertTrue(match, order)
            self.assertEqual(int(match.group(1)), picked)
            tick = int(match.group(2))
            position = self._position(result, picked)
            self.assertLess(math.dist(STAR_BASE_POSITION, position[:2]), math.dist(STAR_BASE_POSITION, CORVETTE_START),
                            position)
            code, hook = self._run(directory, "guard_hook",
                                   (*HUD_OFF, "--eawr-live-order", f"{tick}:guard:{picked}@{STAR_BASE}"), end_tick=450)
            self.assertEqual(code, 0, hook.get("failure"))
            self.assertEqual(hook["live_session"]["final_state_sha256"], live["final_state_sha256"])



    # OR-01: the T and G keys arm the attack-move and guard modes; the next right click uses them.
    def test_t_and_g_arm_attack_move_and_guard(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-modes-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "modes", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", "12:key:T",
                "--eawr-live-input", f"15:rclick:@{DESTINATION[0]},{DESTINATION[1]},0",
                "--eawr-live-input", "40:key:G",
                "--eawr-live-input", f"45:rclick:unit={STAR_BASE}"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            log = battle["log"]
            self.assertEqual((battle["scripted_fired"], battle["orders"]), (5, 2), log)
            self.assertEqual(live["rejected"], [])
            self.assertIn("attack-move mode", log)
            self.assertIn("guard mode", log)
            self.assertTrue(any(line.startswith("attack-move @") for line in log), log)
            self.assertTrue(any(line.startswith(f"guard {STAR_BASE} ") for line in log), log)


    def test_right_click_attacks_a_capital_ship_seen_through_fog(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-ship-") as temporary:
            tick, live = self._attack_through_fog(pathlib.Path(temporary), "attack_ship", TARTAN, TARGET_POINT, 2500, 3300)
            # OR-20: the unit hook aims at collision geometry (#841); only a reticle names a hardpoint.
            self.assertEqual(self.result["battle_input"]["hardpoint_orders"], 0, self.result["battle_input"]["log"])
            hits = [hit for hit in live["first_hits"] if hit["shooter"] == CORVETTE and hit["target"] == TARTAN]
            self.assertEqual(len(hits), 1, live["first_hits"])
            self.assertGreater(hits[0]["tick"], tick, hits)
            # It closed to its slot (0.9 x 800 short of the Tartan) instead of holding 3000 away.
            position = next(unit["position"] for unit in live["own_units"] if unit["entity"] == CORVETTE)
            self.assertLess(math.dist(position[:2], TARGET_POINT), 1000.0, position)


    def test_right_click_on_a_reticle_attacks_that_hardpoint(self):
        # #531 (space-orders OR-20, OR-25; world UI WU-41, WU-42): hovering the Acclamator draws its
        # hardpoint reticles (the Tartan's are not targetable), and the right click on the first one
        # orders the corvette to attack that hardpoint: the order records its index, the session
        # accepts it (where the shots go is the combat contracts' business), the corvette closes and
        # hits the ship, and the reticle flashes. The same order on the hull (the test above) names
        # no hardpoint.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-hardpoint-") as temporary:
            tick, live = self._attack_through_fog(pathlib.Path(temporary), "attack_hardpoint", ACCLAMATOR, TARGET_POINT,
                                                  3000, 3800, reticle=True)
            battle, world = self.result["battle_input"], self.result["world_ui"]
            self.assertEqual(battle["hardpoint_orders"], 1, battle["log"])
            self.assertEqual(world["reticle_flashes"], 1, world)
            self.assertGreater(world["max_reticles"], 0, world)
            self.assertIsNotNone(self.hardpoint, battle["log"])
            hits = [hit for hit in live["first_hits"] if hit["shooter"] == CORVETTE and hit["target"] == ACCLAMATOR]
            self.assertEqual(len(hits), 1, live["first_hits"])
            self.assertGreater(hits[0]["tick"], tick, hits)


    def test_right_click_attacks_a_squadron_seen_through_fog(self):
        # The right click on a TIE Interceptor's craft orders the attack on its squadron, the team
        # container (S-7); the corvette's hits go to the squadron's craft (FO-04).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-attack-squadron-") as temporary:
            tick, live = self._attack_through_fog(pathlib.Path(temporary), "attack_squadron", TIE_SQUADRON, TARGET_POINT,
                                                  2100, 2900)
            hits = [hit for hit in live["first_hits"] if hit["shooter"] == CORVETTE and hit["target"] in TIE_CRAFT]
            self.assertTrue(hits, live["first_hits"])
            self.assertGreater(min(hit["tick"] for hit in hits), tick, hits)


    def test_selection_alone_keeps_the_replay(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-select-") as temporary:
            directory = pathlib.Path(temporary)
            code, idle = self._run(directory, "idle", HUD_OFF)
            self.assertEqual(code, 0, idle.get("failure"))
            box = "@-5250,4550,0/@-4850,4850,0"
            code, result = self._run(directory, "select", (
                *HUD_OFF,
                "--eawr-live-input", f"10:click:unit={CORVETTE}",
                "--eawr-live-input", f"20:dclick:unit={CORVETTE}",
                "--eawr-live-input", f"30:box:{box}",
                "--eawr-live-input", "40:key:1+ctrl",
                "--eawr-live-input", "50:click:@-4700,5100,0",
                "--eawr-live-input", "60:key:1",
                "--eawr-live-input", "65:key:1",
                "--eawr-live-input", "90:key:Insert",
                "--eawr-live-capture-ticks", "35,70,100"))
            self.assertEqual(code, 0, result.get("failure"))
            battle = result["battle_input"]
            self.assertEqual(battle["scripted_fired"], 8, battle["log"])
            self.assertEqual(battle["orders"], 0, battle["log"])
            self.assertGreater(len(battle["selected"]), 1, battle["log"])
            self.assertEqual(battle["groups"]["1"], battle["selected"], battle["log"])
            self.assertEqual(battle["camera_focuses"], 1, battle["log"])
            self.assertEqual(battle["overview"], "overview", battle["log"])
            self.assertIn("box select", battle["log"])
            # UI-C2: selection is presentation state; the session ends where an idle one does.
            self.assertEqual(result["live_session"]["final_state_sha256"], idle["live_session"]["final_state_sha256"])
            self.assertEqual(result["live_session"]["completed_ticks"], idle["live_session"]["completed_ticks"])
            self.assertEqual(self._position(result, CORVETTE), self._position(idle, CORVETTE))


    def test_squadron_selects_and_moves_as_one_unit(self):
        # #424: a click on the Y-wing squadron's icon selects the squadron (its container), a right
        # click moves it, and its craft fly there in formation (FO-01); hovering one craft shows that
        # craft's own health bar and no shield bar (WU-17) while the squadron keeps its three circles.
        destination = (-4550.0, 5150.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-squadron-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "squadron", (
                *HUD_OFF, *SPREAD,
                "--eawr-live-input", f"30:click:icon={Y_WING_SQUADRON}",
                "--eawr-live-input", f"40:rclick:@{destination[0]},{destination[1]},0",
                "--eawr-live-input", f"290:hover:unit={Y_WINGS[1]}"), end_tick=300)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live, world = result["battle_input"], result["live_session"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 3, battle["log"])
            self.assertEqual(battle["selected"], [Y_WING_SQUADRON], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith("move @") and f" units {Y_WING_SQUADRON} " in line
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            for craft in Y_WINGS:
                position = self._position(result, craft)
                self.assertLess(math.dist(destination, position[:2]), 250.0, (craft, position))
            self.assertTrue(world["ring_loaded"] and world["atlas_loaded"], world["unresolved"])
            self.assertEqual(world["circles"], 3, world)
            self.assertTrue(any(re.match(rf"^{Y_WING_SQUADRON}:selected:10:y=", row) for row in world["icon_rows"]),
                            world["icon_rows"])
            self.assertEqual(world["shield_bars"], 0, world)
            # The pointer rests where the hovered craft was drawn. The squadron's icon sits at the
            # squadron's centre and takes the pointer first (WU-23): its own small bar already shows
            # (icon_rows), and #502 no craft gets a bar of its own from that; on a craft the pointer
            # shows that craft's bar alone.
            if battle["hovered_icon"] is not None:
                self.assertEqual(battle["hovered_icon"], Y_WING_SQUADRON, battle)
                self.assertEqual(world["bar_rows"], [], world)
            else:
                self.assertIn(battle["hovered"], Y_WINGS, battle)
                self.assertEqual(world["bar_rows"], [f"{battle['hovered']}:h10"], world)


    def test_double_clicking_an_icon_selects_its_squadron_type(self):
        # #550 (foc-battle-world-ui WU-23a, walk WSU-38): a double click on an X-wing squadron's icon
        # selects every own squadron with a craft of its leader's type (X-Wing) on screen (both of the
        # M2 start's X-wing squadrons), and no squadron of another craft type (the Y-wing squadron and
        # the TIE squadrons stay out). Selection is presentation state: the replay is unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-icon-dclick-") as temporary:
            directory = pathlib.Path(temporary)
            # SPREAD's move takes squadron 3 below a 1280x720 frame of the start camera; here it
            # heads up screen instead, so its craft are well inside the frame at the double click.
            code, result = self._run(directory, "icon-dclick", (
                *HUD_OFF, *SPREAD[:4], "--eawr-live-order", f"1:move:{X_WING_SQUADRON_3}@-4750,5000,0",
                "--eawr-live-input", f"100:dclick:icon={X_WING_SQUADRON}"), end_tick=110)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            self.assertIn("double click: craft type on screen", battle["log"])
            # A wider viewport can also show a squadron the station launched; only X-wing squadrons
            # (the ones with S-foil craft) may join.
            x_wings = {row["container"] for row in live["squadrons"] if row["owner"] == 1 and row["sfoil_craft"] > 0}
            self.assertLessEqual({X_WING_SQUADRON, X_WING_SQUADRON_3}, set(battle["selected"]), battle["log"])
            self.assertLessEqual(set(battle["selected"]), x_wings, battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = {row.split(":")[0]: row for row in result["world_ui"]["icon_rows"]}
            for squadron in (X_WING_SQUADRON, X_WING_SQUADRON_3):
                self.assertIn(":selected:", rows[str(squadron)], rows)
            self.assertIn(":normal:", rows[str(Y_WING_SQUADRON)], rows)


    def test_double_clicking_a_fighter_over_a_ship_selects_no_ship(self):
        # #665, the owner's case (walk WSU-10 to WSU-12, WSU-18): X-wing squadron 3 flies onto the
        # Nebulon-B, and a double click lands just beside one of its craft (35 units above it: a
        # fighter moves between the two presses), over the ship. FoC picks a craft by its collision
        # mesh or else its Mouse_Collide_Override_Sphere_Radius sphere (50 for every M2 craft) and a
        # ship by its collision mesh, highest contact first: the craft wins and the double click adds
        # X-wing squadrons, never the ship. The old per-unit box missed the craft there and hit the
        # ship's box, which is how the ship got in.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-fighter-over-ship-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "fighter-over-ship", (
                *HUD_OFF, "--eawr-live-order", f"1:move:{X_WING_SQUADRON_3}@{NEBULON_START[0]},{NEBULON_START[1]},0",
                "--eawr-live-input", f"150:dclick:unit={X_WING_3_CRAFT}+@0,0,35"), end_tick=160)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            # Every drawn unit has a collision mesh from the unit tables; the craft have spheres.
            self.assertGreater(battle["pick_meshes"], 0, battle)
            self.assertGreater(battle["pick_spheres"], 0, battle)
            self.assertEqual(len(battle["double_click_candidates"]), 1, battle["log"])
            candidates = battle["double_click_candidates"][0]
            # The old box pick: the highest box contact under the pointer was the Nebulon-B's.
            boxes = [row for row in candidates if row["box_z"] is not None]
            self.assertTrue(boxes, candidates)
            self.assertEqual(max(boxes, key=lambda row: row["box_z"])["entity"], NEBULON, candidates)
            # The pick volumes: the craft's sphere is the highest contact.
            volumes = [row for row in candidates if row["contact_z"] is not None]
            winner = max(volumes, key=lambda row: row["contact_z"])
            self.assertEqual((winner["entity"], winner["volume"]), (X_WING_SQUADRON_3, "sphere"), candidates)
            self.assertIn("double click: type on screen", battle["log"], candidates)
            x_wings = {row["container"] for row in live["squadrons"] if row["owner"] == 1 and row["sfoil_craft"] > 0}
            self.assertIn(X_WING_SQUADRON_3, battle["selected"], candidates)
            self.assertLessEqual(set(battle["selected"]), x_wings, battle["log"])
            self.assertIs(live["headless_hashes_equal"], True)


    def test_launched_squadron_selects_and_moves_as_one_unit(self):
        # #518: a squadron the Rebel station launches (SK-23) is a squadron like a tick-zero one: it
        # registers with its whole roster when its container appears, its icon takes a click that
        # selects the container, and a right click moves it as one unit. The Empire's launches
        # register the same way. A probe run finds the station's first launch and its tick.
        destination = (-4300.0, 5000.0)
        with tempfile.TemporaryDirectory(prefix="eawr-battle-launched-") as temporary:
            directory = pathlib.Path(temporary)
            # The station hangar is outside the narrowed 4:3 FOV (e8eed89f); frame its launches.
            launch_camera = (*HUD_OFF, "--eawr-live-follow-group", f"1:{STAR_BASE}")
            code, probe = self._run(directory, "launch-probe", launch_camera, end_tick=LAUNCH_PROBE_TICK)
            self.assertEqual(code, 0, probe.get("failure"))
            rows = probe["live_session"]["squadrons"]
            start = [row["container"] for row in rows if not row["launched"]]
            self.assertEqual(start, [2, 3, 4, TIE_SQUADRON, TIE_SQUADRON + 1], rows)
            launched = [row for row in rows if row["launched"]]
            self.assertTrue(any(row["owner"] == 2 for row in launched), rows)
            for row in launched:
                self.assertIn(len(row["members"]), (3, 4, 5, 7), row)
                self.assertGreater(row["container"], max(row["members"]), row)
            # #632 (foc-battle-world-ui WU-37, WU-38): FoC's small white flag sits on the icon of a
            # squadron the local player's hangar launched; the start's squadrons and the enemy's
            # launched squadrons show none.
            flagged = set(probe["world_ui"]["flags_seen"])
            own_launched = {str(row["container"]) for row in launched if row["owner"] == 1}
            self.assertTrue(own_launched & flagged, (own_launched, flagged))
            self.assertLessEqual(flagged, own_launched, probe["world_ui"])
            self.assertFalse(flagged & {str(container) for container in start}, probe["world_ui"])
            self.assertTrue(probe["world_ui"]["flag_rows"], probe["world_ui"])
            rebel = [row for row in launched if row["owner"] == 1]
            self.assertTrue(rebel, rows)
            squadron = min(rebel, key=lambda row: row["container"])
            tick = squadron["seen_tick"]
            self.assertGreater(tick, 0, squadron)
            code, result = self._run(directory, "launched", (
                *launch_camera,
                "--eawr-live-input", f"{tick + 30}:click:icon={squadron['container']}",
                "--eawr-live-input", f"{tick + 40}:rclick:@{destination[0]},{destination[1]},0"), end_tick=tick + 400)
            self.assertEqual(code, 0, result.get("failure"))
            battle, live, world = result["battle_input"], result["live_session"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["selected"], [squadron["container"]], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith("move @") and f" units {squadron['container']} " in line
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            for craft in squadron["members"]:
                position = self._position(result, craft)
                self.assertLess(math.dist(destination, position[:2]), 250.0, (craft, position))
            self.assertTrue(any(row.startswith(f"{squadron['container']}:selected:") for row in world["icon_rows"]), world)
            self.assertEqual(world["circles"], len(squadron["members"]), world)


    def test_launched_squadron_locks_its_sfoils(self):
        # #614 (space-abilities AB-31): the S-foil button of a squadron the Rebel station launches
        # locks its craft's S-foils like a tick-zero squadron's: the order reaches every flying craft
        # and each craft plays its DEPLOY clip. The launched craft draw in launch slots, which used
        # to get no S-foil clips (the button was clickable and nothing moved). X-wing squadron 2 is
        # the reference; the station's first launch is an X-wing squadron (SK-23).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-launched-sfoils-") as temporary:
            directory = pathlib.Path(temporary)

            def squadron_row(result, container):
                rows = [row for row in result["live_session"]["squadrons"] if row["container"] == container]
                self.assertEqual(len(rows), 1, result["live_session"]["squadrons"])
                return rows[0]

            def click_sfoil(name, container, tick):
                # SPREAD moves the stacked start squadrons apart, so an icon click picks its own squadron.
                code, result = self._run(directory, name, (
                    *SPREAD,
                    # Keep the launched roster on screen after the 4:3 FOV correction (e8eed89f).
                    "--eawr-live-follow-group", f"1:{container if tick else X_WING_SQUADRON}",
                    "--eawr-live-input", f"{tick + 30}:click:icon={container}",
                    "--eawr-live-input", f"{tick + 40}:click:ability=0"), end_tick=tick + 160)
                self.assertEqual(code, 0, result.get("failure"))
                battle, live = result["battle_input"], result["live_session"]
                self.assertEqual(battle["scripted_fired"], 2, battle["log"])
                self.assertEqual(battle["selected"], [container], battle["log"])
                self.assertEqual(live["ability_requests"]["issued"], 1, live["ability_requests"])
                self.assertEqual(live["rejected"], [])
                self.assertIs(live["headless_hashes_equal"], True)
                return result, squadron_row(result, container)

            start, start_row = click_sfoil("start-sfoils", X_WING_SQUADRON, 0)
            crafts = len(start_row["members"])
            self.assertGreater(crafts, 0, start_row)
            self.assertEqual((start_row["sfoil_craft"], start_row["sfoils_locked"]), (crafts, crafts), start_row)
            self.assertEqual(start["live_session"]["sfoil_switches"], crafts)

            code, probe = self._run(directory, "launch-probe", HUD_OFF, end_tick=LAUNCH_PROBE_TICK)
            self.assertEqual(code, 0, probe.get("failure"))
            rebel = [row for row in probe["live_session"]["squadrons"] if row["launched"] and row["owner"] == 1]
            self.assertTrue(rebel, probe["live_session"]["squadrons"])
            squadron = min(rebel, key=lambda row: row["container"])
            launched, launched_row = click_sfoil("launched-sfoils", squadron["container"], squadron["seen_tick"])
            crafts = len(launched_row["members"])
            self.assertEqual((launched_row["sfoil_craft"], launched_row["sfoils_locked"]), (crafts, crafts), launched_row)
            self.assertEqual(launched["live_session"]["sfoil_switches"], crafts)


    def test_named_hero_world_icon_selects_the_ship_and_respects_fog(self):
        # WU-47/48: Thrawn has named identity without an explicit Show_Hero_Head.
        with tempfile.TemporaryDirectory(prefix="eawr-hero-world-icon-") as temporary:
            directory = pathlib.Path(temporary)
            stage = (*HUD_OFF, "--eawr-live-ai", "off", "--eawr-live-workers", "4",
                     "--eawr-skirmish-slot", "1:Empire:0:human",
                     "--eawr-skirmish-slot", "2:Rebel:1:ai",
                     "--eawr-skirmish-fleet", "1:Admonitor_Star_Destroyer",
                     "--eawr-skirmish-fleet", "2:none",
                     "--eawr-live-follow-group", "1:4")
            code, shown = self._run(directory, "hero-normal", stage, session="skirmish", camera=None, end_tick=40)
            self.assertEqual(code, 0, shown.get("failure"))
            squadrons = {row["squadron"] for row in shown["world_ui"]["gripper_points"]}
            heads = [row for row in shown["world_ui"]["icon_rows"] if int(row.split(":")[0]) not in squadrons]
            self.assertEqual(len(heads), 1, shown["world_ui"])
            hero = int(heads[0].split(":")[0])
            self.assertIn(hero, {row["entity"] for row in shown["live_session"]["own_units"]})
            code, selected = self._run(directory, "hero-selected", (*stage,
                "--eawr-live-input", f"20:click:icon={hero}"), session="skirmish", camera=None, end_tick=40)
            self.assertEqual(code, 0, selected.get("failure"))
            self.assertEqual(selected["battle_input"]["selected"], [hero])
            self.assertTrue(any(row.startswith(f"{hero}:selected:") for row in selected["world_ui"]["icon_rows"]))
            self.assertTrue(selected["live_session"]["headless_hashes_equal"])
            code, fogged = self._run(directory, "hero-enemy-fog", (*stage,
                "--eawr-live-player", "2"), session="skirmish", camera=None, end_tick=40)
            self.assertEqual(code, 0, fogged.get("failure"))
            self.assertFalse(any(row.startswith(f"{hero}:") for row in fogged["world_ui"]["icon_rows"]))

    def test_squadron_icon_slides_toward_a_moving_formation(self):
        # WSU-34, #661: an ordered squadron's icon accelerates toward its world
        # anchor rather than sticking to its published centre on every snapshot.
        with tempfile.TemporaryDirectory(prefix="eawr-gripper-slide-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "slide", (
                *HUD_OFF, "--eawr-live-ai", "off", "--eawr-live-step", "10",
                "--eawr-live-order", f"1:move:{X_WING_SQUADRON}@-4057,4700,0"), end_tick=100)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            points = {row["squadron"]: row for row in result["world_ui"]["gripper_points"]}
            self.assertIn(X_WING_SQUADRON, points)
            anchor = points[X_WING_SQUADRON]
            centre = self._position(result, X_WING_SQUADRON)
            self.assertGreater(math.dist(anchor["position"], centre), 10, (anchor, centre))
            self.assertGreater(anchor["speed"], 0, anchor)
            # The X-wing's XML maximum speed bounds every member's current speed.
            self.assertLessEqual(anchor["speed"], 5.21, anchor)

    def test_dogfighting_squadrons_share_a_grid_cell(self):
        # #435: X-wing squadron 2 and TIE Interceptor squadron 9 meet mid-map and attack each other.
        # The orders target the team containers (space-fighters FO-04); the craft fire at the other
        # squadron's craft, and once they dogfight both squadrons hold one combat cell, whose icons
        # sit in its grid (foc-battle-world-ui WU-25, WU-26). TIE Interceptor squadron 10, the M2
        # start's other idle Empire squadron, is sent well clear first (OTHER_TIE_SQUADRON) so #511's
        # fire-reveals-the-shooter fog (V-19) cannot let its own idle scan (FT-02) pull it into the
        # same cell once the dogfight starts shooting.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-") as temporary:
            directory = pathlib.Path(temporary)
            meeting = f"{MEETING[0]},{MEETING[1]},0"
            clear = f"{CLEAR_OF_DOGFIGHT[0]},{CLEAR_OF_DOGFIGHT[1]},0"
            # The live camera over the meeting point: the view draws (and so points at) what it sees.
            camera = directory / "dogfight-camera.xml"
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            camera.write_text(re.sub(r"<initial [^>]*/>", f'<initial target_x="{MEETING[0]}" target_y="{MEETING[1]}" '
                                     'target_height="0" zoom="0.5" yaw_degrees="0"/>', CAMERA.read_text(encoding="utf-8")),
                              encoding="utf-8")
            code, result = self._run(directory, "dogfight", (
                *HUD_OFF, "--eawr-live-ai", "off",
                "--eawr-live-order", f"1:move:{X_WING_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{TIE_SQUADRON}@{meeting}",
                "--eawr-live-order", f"1:move:{OTHER_TIE_SQUADRON}@{clear}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{X_WING_SQUADRON}@{TIE_SQUADRON}",
                "--eawr-live-order", f"{DOGFIGHT_ORDER_TICK}:attack:{TIE_SQUADRON}@{X_WING_SQUADRON}"),
                camera=camera, end_tick=DOGFIGHT_TICK)
            self.assertEqual(code, 0, result.get("failure"))
            live, world = result["live_session"], result["world_ui"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = {row.split(":")[0]: row for row in world["icon_rows"]}
            cells = {}
            for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
                found = re.search(r":grid(-?\d+,-?\d+)$", rows.get(str(squadron), ""))
                self.assertTrue(found, world["icon_rows"])
                cells[squadron] = found.group(1)
            self.assertEqual(cells[X_WING_SQUADRON], cells[TIE_SQUADRON], world["icon_rows"])
            self.assertGreaterEqual(world["max_grid_icons"], 2, world)
            # Squadrons not in a dogfight keep their icons over themselves.
            self.assertTrue(all(":grid" not in row for key, row in rows.items()
                                if key not in (str(X_WING_SQUADRON), str(TIE_SQUADRON))), world["icon_rows"])
            # WSU-36: two joined squadrons in this cell occupy the same row at
            # their fixed slots, without the sliding anchor's screen offset.
            ys = {}
            for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
                found = re.search(r":y=(-?\d+)", rows[str(squadron)])
                self.assertTrue(found, world["icon_rows"])
                ys[squadron] = int(found.group(1))
            self.assertEqual(ys[X_WING_SQUADRON], ys[TIE_SQUADRON], world["icon_rows"])


    def test_the_dogfight_grid_holds_still_while_the_dogfight_holds(self):
        # #564 (WU-26, debug build): the grid's cell point sits at the craft type's Layer_Z_Adjust, a
        # constant, so the icons in a held cell do not bob as the fighters pitch. The same dogfight
        # read at three later ticks: one cell, and every gridded icon on the same screen Y.
        seen = []
        for end_tick in (DOGFIGHT_TICK, DOGFIGHT_TICK + 25, DOGFIGHT_TICK + 50):
            with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-still-") as temporary:
                code, result = self._dogfight_run(pathlib.Path(temporary), "dogfight-still", (), end_tick=end_tick)
                self.assertEqual(code, 0, result.get("failure"))
                rows = {row.split(":")[0]: row for row in result["world_ui"]["icon_rows"]}
                seen.append({squadron: re.search(r":y=(-?\d+):grid(-?\d+,-?\d+)$", rows[str(squadron)])
                             for squadron in (X_WING_SQUADRON, TIE_SQUADRON)})
        for tick, found in zip((0, 25, 50), seen):
            self.assertTrue(all(found.values()), (tick, found))
        cells = {match.group(2) for found in seen for match in found.values()}
        self.assertEqual(len(cells), 1, cells)
        for squadron in (X_WING_SQUADRON, TIE_SQUADRON):
            ys = [found[squadron].group(1) for found in seen]
            self.assertEqual(len(set(ys)), 1, (squadron, ys))


    def test_right_clicking_an_enemy_squadron_icon_attacks_the_squadron(self):
        # #553 (WU-23b, FO-04): with X-wing squadron 2 selected by its icon, a right click on the TIE
        # squadron's icon in the dogfight's grid cell orders the attack on that squadron's container.
        with tempfile.TemporaryDirectory(prefix="eawr-battle-icon-rclick-") as temporary:
            code, result = self._dogfight_run(pathlib.Path(temporary), "icon-rclick", (
                f"{DOGFIGHT_TICK - 30}:click:icon={X_WING_SQUADRON}",
                f"{DOGFIGHT_TICK - 20}:rclick:icon={TIE_SQUADRON}"))
            self.assertEqual(code, 0, result.get("failure"))
            battle, live = result["battle_input"], result["live_session"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["selected"], [X_WING_SQUADRON], battle["log"])
            self.assertEqual(battle["orders"], 1, battle["log"])
            self.assertTrue(any(line.startswith(f"attack {TIE_SQUADRON} units {X_WING_SQUADRON} ")
                                for line in battle["log"]), battle["log"])
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)


    def test_double_clicking_a_dogfighting_squadrons_grid_icon(self):
        # #550 (WU-23a, WU-25, WU-26): the dogfight above, then a double click on X-wing squadron 2's
        # icon in the combat cell's grid. The icon takes the click where it is drawn: squadron 2 is
        # selected and the TIE squadron sharing its cell is not (another craft type); X-wing squadron
        # 3 is still at the spawn stack, far off this camera, so none of its craft is on screen and it
        # is not added (WSU-38).
        with tempfile.TemporaryDirectory(prefix="eawr-battle-dogfight-dclick-") as temporary:
            code, result = self._dogfight_run(pathlib.Path(temporary), "dogfight-dclick",
                                              (f"{DOGFIGHT_TICK - 10}:dclick:icon={X_WING_SQUADRON}",))
            self.assertEqual(code, 0, result.get("failure"))
            battle, world = result["battle_input"], result["world_ui"]
            self.assertEqual(battle["scripted_fired"], 1, battle["log"])
            self.assertEqual(battle["selected"], [X_WING_SQUADRON], battle["log"])
            rows = {row.split(":")[0]: row for row in world["icon_rows"]}
            self.assertRegex(rows[str(X_WING_SQUADRON)], r":selected:.*:grid-?\d+,-?\d+$")
            self.assertIn(":normal:", rows[str(TIE_SQUADRON)], rows)
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)


    def test_bad_inputs_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="eawr-battle-bad-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "unknown", ("--eawr-live-input", "5:click:unit=999"))
            self.assertNotEqual(code, 0)
            self.assertIn("is not a unit of the start", result["failure"])
            code, result = self._run(directory, "malformed", ("--eawr-live-input", "5:jump:unit=5"))
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-live-input expects", result["failure"])
