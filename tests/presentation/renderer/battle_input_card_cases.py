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


class BattleInputCardCases:
    def test_bought_ship_shows_stun_damage_and_power_to_weapons(self):
        # #1447 / BP-43 / IS-09: purchase slots must read their occupying unit's
        # snapshot, including damage and abilities; initial-fleet cases miss this.
        import xml.etree.ElementTree as ET
        import struct
        from tools.inventory.corpus import Corpus

        with tempfile.TemporaryDirectory(prefix="eawr-bought-emitter-") as temporary:
            directory = pathlib.Path(temporary)
            map_path = "data/art/maps/_mp_space_geonosis.ted"
            xml = directory / "menu/Data/XML"
            xml.mkdir(parents=True)
            (xml.parent / "MegaFiles.xml").write_text(
                "<Mega_Files><File>Absent.meg</File></Mega_Files>", encoding="utf-8")
            corpus = Corpus(os.environ["EAWR_EAW_GAME_ROOT"])
            # Isolate an authored Acclamator purchase, as the carrier-arrival
            # case does, without first spending the GPU run on station upgrades.
            for filename in ("starbases.xml", "gameconstants.xml"):
                source = corpus.read_effective("foc", "data/xml/" + filename)
                self.assertIsNotNone(source)
                tree = ET.fromstring(source.data)
                if filename == "starbases.xml":
                    station = next(node for node in tree
                                   if node.attrib.get("Name") == "Skirmish_Empire_Star_Base_1")
                    for child in list(station):
                        if child.tag == "Tactical_Buildable_Objects_Multiplayer":
                            station.remove(child)
                    ET.SubElement(station, "Tactical_Buildable_Objects_Multiplayer").text = (
                        "Empire, Acclamator_Assault_Ship")
                else:
                    next(node for node in tree.iter()
                         if node.tag == "MP_Default_Start_Tech_Level").text = "3"
                (xml / filename).write_bytes(ET.tostring(tree, encoding="utf-8"))
            common = ["--eawr-mod-root", str(xml.parents[1]), "--eawr-live-ai", "off",
                      "--eawr-live-player", "1", "--eawr-live-follow", "1",
                      "--eawr-live-reveal", "on", "--eawr-live-step", "0.5",
                      "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                      "--eawr-skirmish-slot", "1:Empire:0:human",
                      "--eawr-skirmish-slot", "2:Rebel:1:ai",
                      "--eawr-skirmish-fleet", "1:none",
                      "--eawr-skirmish-fleet", "2:Y-Wing_Squadron",
                      "--eawr-live-input", "10:click:unit=1"]
            seed = directory / "setup.eawr-replay"
            code, menu = self._run(directory, "menu", common + [
                "--eawr-live-replay-out", str(seed)], camera=None,
                                   end_tick=30, session="skirmish", map_path=map_path)
            self.assertEqual(code, 0, menu.get("failure"))
            card = menu["hud"]["unit_cards"]["drawn"][0]
            self.assertEqual(card["type"], "Acclamator_Assault_Ship")
            bomber = next(row["entity"] for row in menu["live_session"]["start_fleet"]
                          if row["player"] == 2 and row["type"] == "Y-Wing_Squadron")
            station = self._position(menu, 1)
            point = (station[0] + 1800, station[1] - 500)
            # Stage the bomber clear of the station and next to the future
            # reinforcement. Long travel otherwise lets unrelated fighters
            # intercept its ability bolts before they reach the intended hull.
            staged = bytearray(seed.read_bytes())
            start_offset = struct.unpack_from("<H", staged, 10)[0] + 24 * struct.unpack_from("<I", staged, 48)[0]
            squadron = next(row for row in menu["live_session"]["squadrons"] if row["container"] == bomber)
            members = {bomber, *squadron["members"]}
            for index in range(struct.unpack_from("<Q", staged, 56)[0]):
                offset = start_offset + 80 * index
                if struct.unpack_from("<Q", staged, offset)[0] in members:
                    struct.pack_into("<3q", staged, offset + 24,
                                     round((point[0] + 900) * (1 << 24)), round(point[1] * (1 << 24)), 0)
            seed.write_bytes(staged)
            ready = 20 + card["build_frames"] + 60
            placement = ready + 20
            replay_common = common[:common.index("--eawr-skirmish-slot")]
            purchase = replay_common + ["--eawr-live-replay", str(seed),
                "--eawr-live-input", "10:click:unit=1", "--eawr-live-input", "20:click:card=0",
                "--eawr-live-order", f"1:move:{bomber}@{point[0] + 900},{point[1]},0",
                "--eawr-live-input", f"{ready}:click:hud=b_reinforcement",
                "--eawr-live-input", f"{ready + 10}:press:hud=r_0000",
                "--eawr-live-input", f"{ready + 15}:hover:@{point[0]},{point[1]},0",
                "--eawr-live-input", f"{placement}:release:@{point[0]},{point[1]},0"]
            replay = directory / "bought.eawr-replay"
            code, arrival = self._run(directory, "arrival", purchase + [
                "--eawr-live-replay-out", str(replay)], camera=None,
                                      end_tick=placement + 170, session="replay", map_path=map_path)
            self.assertEqual(code, 0, arrival.get("failure"))
            arrivals = [row for row in arrival["live_session"]["arrivals"] if row["owner"] == 1]
            self.assertEqual(len(arrivals), 1, arrivals)
            self.assertEqual(arrival["live_session"]["economy_requests"]["buys"], 1)
            self.assertEqual(arrival["live_session"]["economy_requests"]["reinforcements"], 1)
            self.assertTrue(arrival["live_session"]["headless_hashes_equal"])
            self.assertEqual(arrival["live_session"]["rejected"], [])
            unit, landed = arrivals[0]["unit"], arrivals[0]["landed_tick"]
            self.assertIsNotNone(landed, arrivals)
            damage_tick = arrivals[0]["visible_tick"] + 1
            power_tick, ion_tick = landed + 60, landed + 180
            # The initial-unit CLI selectors cannot name a future target. Append
            # commands to this run's own recording (docs/replay-format.md), keeping
            # its installed identity, actual purchase and reinforcement commands.
            data = bytearray(replay.read_bytes())
            self.assertEqual(data[:8], b"EAWRPLY\0")
            header = struct.unpack_from("<H", data, 10)[0]
            players = struct.unpack_from("<I", data, 48)[0]
            units = struct.unpack_from("<Q", data, 56)[0]
            count = struct.unpack_from("<Q", data, 64)[0]
            offset = header + 24 * players + 80 * units
            for _ in range(struct.unpack_from("<I", data, 52)[0]):
                members = struct.unpack_from("<I", data, offset + 8)[0]
                offset += 16 + 8 * members
            sequence = 0
            for _ in range(count):
                length = struct.unpack_from("<I", data, offset)[0]
                sequence = max(sequence, struct.unpack_from("<Q", data, offset + 16)[0])
                offset += 4 + length
            self.assertEqual(offset, len(data))
            commands = []

            def command(tick, player, opcode, payload, source):
                body = struct.pack("<QIQBBH", tick, player, sequence + len(commands) + 1, opcode, 0, 0)
                body += payload + struct.pack("<IIQ", 1, 0, source)
                commands.append(struct.pack("<I", len(body)) + body)

            # Disable the bay before landing so no launched craft intercepts
            # the ion shot. Engines remain intact; later gun losses show damage.
            command(damage_tick, 1, 4,
                    struct.pack("<qII", 100000 << 24, 7, 0), unit)
            for hardpoint in range(6):
                command(landed + 30, 1, 4, struct.pack("<qII", 100000 << 24, hardpoint, 0), unit)
            command(power_tick, 1, 8, struct.pack("<BBHI", 3, 1, 0, 0), unit)
            command(power_tick + 90, 1, 8, struct.pack("<BBHI", 3, 2, 0, 0), unit)
            command(ion_tick, 2, 8, struct.pack("<BBHIQ", 5, 1, 1, 0xffffffff, unit), bomber)
            end = ion_tick + 800
            struct.pack_into("<Q", data, 40, end)
            struct.pack_into("<Q", data, 64, count + len(commands))
            replay.write_bytes(data + b"".join(commands))
            effects = replay_common + ["--eawr-live-replay", str(replay),
                        "--eawr-live-follow-group", f"{landed}:{unit}",
                        "--eawr-live-capture-ticks", f"{damage_tick + 20},{power_tick + 30},{ion_tick + 130}"]
            # Follow the bought hull rather than the station used for purchase.
            follow = effects.index("--eawr-live-follow")
            del effects[follow:follow + 2]
            code, result = self._run(directory, "bought-effects", effects, camera=None,
                                     end_tick=end, session="replay", map_path=map_path)
            self.assertEqual(code, 0, result.get("failure"))
            live, emitters = result["live_session"], result["unit_emitters"]
            self.assertEqual(live["rejected"], [])
            self.assertTrue(live["headless_hashes_equal"])
            self.assertIn(unit, {row["unit"] for row in live["arrivals"]})
            ion = "Proj_Ion_Cannon_Medium_Laser_Blue"
            self.assertGreater(result["battle_effects"]["ability_shot_frames_drawn"].get(ion, 0), 0)
            stunned = {row["unit"]: row for row in live["ion_shots"]["stunned"]}
            self.assertIn(unit, stunned, live["ion_shots"])
            self.assertGreater(stunned[unit]["max_frames"], 0)
            self.assertFalse(emitters["start_log_full"])
            self.assertEqual(emitters["start_failed"], {})
            rows = [row for row in emitters["start_log"] if row["unit"] == unit]
            for prefix, first in (("p_hp_", damage_tick), ("pptw_", power_tick),
                                  ("pi_", stunned[unit]["first"])):
                with self.subTest(effect=prefix):
                    starts = [row for row in rows if row["proxy"].lower().startswith(prefix)]
                    self.assertTrue(starts, rows)
                    self.assertTrue(all(row["tick"] >= first for row in starts), starts)
                    self.assertTrue(any(row["presented"] > 0 for row in starts), starts)
            self.assertGreater(emitters["ion_stun_population"]["max_particles"], 0)
            self.assertGreater(emitters["power_to_weapons_drains"]["finished"], 0)

    def test_unit_cards_show_and_change_the_selection(self):
        # #425 (docs/behaviour/foc-unit-cards.md): the box's selection fills the command bar's cards,
        # one per unit or squadron, grouped by ability; a card click goes through the HUD (never the
        # world layer) and selects like FoC's Tactical_Select_Object, Shift+click deselects, a double
        # click runs the click again, and a hovered card names its type after Encyclopedia_Delay.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-cards-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "cards", (*boxed, "--eawr-live-input", "25:hover:card=0"), end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            battle, hud = shown["battle_input"], shown["hud"]
            cards = battle["unit_cards"]
            self.assertEqual(cards["slots"], 24)
            self.assertGreaterEqual(cards["groups"], 2, cards)
            self.assertIn(CORVETTE, battle["selected"], battle["log"])
            by_slot = {card["slot"]: card for card in cards["cards"]}
            # Every selected unit is on exactly one card; squadron craft only through their squadron.
            members = sorted(member for card in cards["cards"] for member in card["members"])
            self.assertEqual(members, sorted(battle["selected"]))
            self.assertTrue(any(card["squadron"] for card in cards["cards"]), cards)
            # #435: the selection holds a squadron as its container (#424); its one card stands for
            # the container, and selecting the card selects the squadron as one unit.
            for card in cards["cards"]:
                if card["squadron"]:
                    self.assertEqual(card["members"], [card["unit"]], card)
                    self.assertEqual(sum(1 for other in cards["cards"] if card["unit"] in other["members"]), 1, cards)
            # Groups follow UnitAbilityType order, each from a new column.
            abilities = [by_slot[slot]["ability"] for slot in sorted(by_slot)]
            self.assertEqual(abilities, sorted(abilities))
            for previous, card in zip(sorted(by_slot), sorted(by_slot)[1:]):
                if by_slot[card]["ability"] != by_slot[previous]["ability"]:
                    self.assertEqual(card % 2, 0, cards)
            drawn = {card["slot"]: card for card in hud["unit_cards"]["drawn"]}
            self.assertEqual(sorted(drawn), sorted(by_slot))
            for card in drawn.values():
                self.assertTrue(card["icon_drawn"], card)
            tooltip = hud["unit_cards"]["tooltip"]
            self.assertIsNotNone(tooltip, hud["unit_cards"])
            self.assertEqual(tooltip["slot"], 0)
            self.assertTrue(tooltip["text"], tooltip)
            # This suite runs without a font cache (engine-font warnings); every card texture resolves.
            card_icons = {card["icon"] for card in drawn.values()}
            self.assertFalse([line for line in hud["diagnostics"] if "EAWR-UI-0322" in line
                              and any(icon in line for icon in card_icons)], hud["diagnostics"])

            first = by_slot[0]
            group = sorted(member for card in cards["cards"] if card["ability"] == first["ability"]
                           for member in card["members"])
            code, picked = self._run(directory, "card-click", (*boxed, "--eawr-live-input", "30:click:card=0"),
                                     end_tick=60)
            self.assertEqual(code, 0, picked.get("failure"))
            self.assertEqual(sorted(picked["battle_input"]["selected"]), group, picked["battle_input"]["log"])
            self.assertEqual(picked["hud"]["unit_cards"]["clicks"], 1)
            # The HUD took the card click: only the box's press and release reached the world layer.
            self.assertEqual(picked["battle_input"]["events"], 2, picked["battle_input"]["log"])
            # UI-C2: a card click is presentation only.
            self.assertEqual(picked["live_session"]["final_state_sha256"], shown["live_session"]["final_state_sha256"])

            squadron = next(card for card in cards["cards"] if card["squadron"])
            # #435: a squadron card's click selects containers, never craft (the M2 Rebel craft are
            # 36-48), and the order that follows goes to those containers and is accepted.
            code, ordered = self._run(directory, "card-order", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron['slot']}",
                "--eawr-live-input", f"35:rclick:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, ordered.get("failure"))
            chosen = ordered["battle_input"]["selected"]
            self.assertIn(squadron["unit"], chosen, ordered["battle_input"]["log"])
            self.assertTrue(set(chosen).isdisjoint(range(36, 49)), chosen)
            units = ",".join(str(entity) for entity in chosen)
            self.assertTrue(any(line.startswith("move @") and f" units {units} " in line
                                for line in ordered["battle_input"]["log"]), ordered["battle_input"]["log"])
            self.assertEqual(ordered["live_session"]["rejected"], [])
            code, dropped = self._run(directory, "card-shift", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron['slot']}+shift"), end_tick=60)
            self.assertEqual(code, 0, dropped.get("failure"))
            self.assertEqual(sorted(dropped["battle_input"]["selected"]),
                             sorted(set(battle["selected"]) - set(squadron["members"])), dropped["battle_input"]["log"])

            code, twice = self._run(directory, "card-dclick", (*boxed, "--eawr-live-input", "30:dclick:card=0"),
                                    end_tick=60)
            self.assertEqual(code, 0, twice.get("failure"))
            self.assertEqual(twice["hud"]["unit_cards"]["double_clicks"], 1)
            self.assertEqual(twice["hud"]["unit_cards"]["clicks"], 2)
            # C-1, C-2: the first action selects the card's ability group (the Nebulon-B and the MC80
            # share DEFEND); the second runs on that one-group selection and selects the card's own
            # unit (or, on a collapsed stack, its type's card units).
            self.assertEqual(sorted(twice["battle_input"]["selected"]), sorted(first["members"]))



    # #862 (space-abilities AB-11, AB-60 to AB-67; foc-ability-buttons AB-11): the Y-wing
    # squadron's ion shot as the owner plays it, with fog on and the AI off so the Empire stays
    # where the debug moves put it. The Nebulon-B spots the Tartan (as in _attack_through_fog)
    # and the squadron waits 900 units from it with the camera on it. A click on the squadron's
    # icon selects it; its ION_CANNON_SHOT button (or Shift+I) waits for a target; a click on the
    # Tartan sends the shot. Each Y-wing fires the override projectile once at the Tartan, which
    # the viewer draws with that projectile's own look (battle-presentation BP-66), and the hits
    # stun it (space-damage IS-01). The Tartan's guns take the squadron down afterwards.
    def test_ion_shot_button_target_click_fires_and_stuns(self):
        ion = "Proj_Ion_Cannon_Medium_Laser_Blue"
        # WSU-34 slides the icon anchor once per drawn frame; use the viewer's
        # normal two drawn frames per simulation tick (TP-06), including emitter presentation.
        setup = ("--eawr-live-ai", "off", "--eawr-live-step", "0.5",
                 "--eawr-live-order", f"1:move:{NEBULON}@{SPOTTER_POINT[0]},{SPOTTER_POINT[1]},0",
                 "--eawr-live-order", f"1:move:{TARTAN}@{TARGET_POINT[0]},{TARGET_POINT[1]},0",
                 "--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-1900,1750,0",
                 "--eawr-live-follow", str(Y_WING_SQUADRON),
                 "--eawr-live-input", f"2200:click:icon={Y_WING_SQUADRON}")
        aim = ("--eawr-live-input", f"2220:click:unit={TARTAN}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-ion-shot-") as temporary:
            directory = pathlib.Path(temporary)
            for name, press in (("button", "2210:click:ability=0"), ("hotkey", "2210:key:I+shift")):
                code, result = self._run(directory, f"ion-{name}", (*setup, "--eawr-live-input", press, *aim),
                                         end_tick=2600)
                self.assertEqual(code, 0, result.get("failure"))
                battle, live, effects = result["battle_input"], result["live_session"], result["battle_effects"]
                log = battle["log"]
                bar = battle["ability_bar"]
                self.assertEqual((bar["requests"], bar["targeted"], bar["target_cancels"], bar["targeting"]),
                                 (1, 1, 0, False), log)
                self.assertEqual(bar["hotkeys"], 1 if name == "hotkey" else 0, log)
                self.assertIn("ability ION_CANNON_SHOT: pick a target", log)
                self.assertIn(f"ability ION_CANNON_SHOT at {TARTAN}", log)
                self.assertEqual(live["ability_requests"], {"issued": 1, "refused": 0})
                self.assertEqual(live["rejected"], [])
                self.assertIs(live["headless_hashes_equal"], True)
                # AB-63 to AB-65: the squadron's shot switched on once, after the click, and ended.
                shots = {row["squadron"]: row for row in live["ion_shots"]["squadrons"]}
                self.assertEqual(shots[Y_WING_SQUADRON]["switched_on"], 1, shots)
                self.assertGreaterEqual(shots[Y_WING_SQUADRON]["first_on"], 2220, shots)
                self.assertFalse(shots[Y_WING_SQUADRON]["on"], shots)
                # AB-66, AB-67: one override bolt per Y-wing, drawn as the override projectile.
                fired = effects["ability_shots_fired"]
                self.assertEqual(list(fired), [ion], effects)
                self.assertTrue(1 <= fired[ion] <= len(Y_WINGS), fired)
                self.assertGreater(effects["ability_shot_frames_drawn"].get(ion, 0), 0, effects)
                # IS-01, IS-03: the bolts stun the Tartan.
                stunned = {row["unit"]: row for row in live["ion_shots"]["stunned"]}
                self.assertIn(TARTAN, stunned, live["ion_shots"])
                self.assertGreater(stunned[TARTAN]["max_frames"], 0, stunned)
                self.assertGreater(stunned[TARTAN]["first"], shots[Y_WING_SQUADRON]["first_on"], stunned)
                # IS-09: the stun shows the Tartan's authored-hidden ion-stun emitter on its hull,
                # from the stun on; when the stun ends it stops emitting and drains.
                emitters = result["unit_emitters"]
                started = {name.lower(): count for name, count in emitters["started"].items()}
                self.assertGreater(started.get("pi_damage_elec_cap00", 0), 0, emitters["started"])
                ion_rows = [row for row in emitters["start_log"]
                            if row["unit"] == TARTAN and row["proxy"].lower().startswith("pi")]
                if not emitters["start_log_full"]:
                    self.assertTrue(ion_rows, emitters["start_log"])
                for row in ion_rows:
                    self.assertGreaterEqual(row["tick"], stunned[TARTAN]["first"], row)
                    self.assertGreater(row["presented"], 0, row)
                self.assertGreater(emitters["ion_stun_drains"]["started"], 0, emitters["ion_stun_drains"])


    def test_victory_ion_stun_retains_authored_particle_population_lit(self):
        # #1187: a stream-start counter alone missed the 256-particle pool
        # dropping most of this 3500/s, half-second hull effect's births.
        victory = 13
        inputs = ("--eawr-skirmish-fleet", "2:Tartan_Patrol_Cruiser,Acclamator_Assault_Ship,Victory_Destroyer",
                  "--eawr-live-ai", "off", "--eawr-live-reveal", "on", "--eawr-live-player", "2",
                  "--eawr-live-step", "2", "--eawr-environment", "map", "--eawr-lighting", "sh",
                  "--eawr-shadows", "on", "--eawr-live-follow", str(victory),
                  "--eawr-live-order", f"1:move:{victory}@-2000,1750,0",
                  "--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-2900,1750,0",
                  "--eawr-live-order", f"3000:ability:{Y_WING_SQUADRON}@ION_CANNON_SHOT,on,{victory}",
                  "--eawr-live-capture-ticks", "3640,3680,3700,3740")
        with tempfile.TemporaryDirectory(prefix="eawr-victory-ion-lit-") as temporary:
            directory = pathlib.Path(os.environ.get("EAWR_VICTORY_ION_CAPTURE_DIR") or temporary).resolve()
            directory.mkdir(parents=True, exist_ok=True)
            code, result = self._run(directory, "victory-ion-lit", inputs, camera=None,
                                     end_tick=3900, session="skirmish")
            self.assertEqual(code, 0, result.get("failure"))
            live, emitters = result["live_session"], result["unit_emitters"]
            self.assertTrue(live["headless_hashes_equal"])
            self.assertIn(victory, {row["unit"] for row in live["ion_shots"]["stunned"]})
            started = {name.lower(): count for name, count in emitters["started"].items()}
            self.assertGreater(started.get("pi_damage_elec_sd00", 0), 0)
            self.assertEqual(emitters["start_failed"], {})
            population = emitters["ion_stun_population"]
            self.assertGreaterEqual(population["max_particles"], 1700, population)
            self.assertLessEqual(population["max_particles"], 2000, population)
            self.assertEqual(population["dropped_at_capacity"], 0, population)
            self.assertEqual(emitters["ion_stun_drains"], {"started": 1, "finished": 1, "cut_short": 0})
            for tick in (3640, 3680, 3700, 3740):
                self.assertTrue((directory / f"victory-ion-lit_t{tick}.png").is_file())

    def test_world_ability_overlays_and_control_group_three(self):
        # #768 / WU-43..WU-46: actual battle gestures share card state with both world surfaces.
        lit = ("--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
        inputs = (*lit, *MC80_ASIDE, *Y_WING_ASIDE,
                  "--eawr-live-order", "1:move:3@-4650,4250,0",
                  "--eawr-live-input", "100:click:icon=2",
                  "--eawr-live-input", "110:key:W+shift",
                  "--eawr-live-input", "120:key:3+ctrl",
                  "--eawr-live-input", "150:click:unit=6",
                  "--eawr-live-input", "160:key:O+shift",
                  "--eawr-live-input", "170:key:3+alt")
        destination = os.environ.get("EAWR_ABILITY_OVERLAY_CAPTURE_DIR")
        with tempfile.TemporaryDirectory(prefix="eawr-world-ability-") as temporary:
            directory = pathlib.Path(destination or temporary).resolve()
            directory.mkdir(parents=True, exist_ok=True)
            code, result = self._run(directory, "ability-overlay-on-lit", inputs, end_tick=210)
            self.assertEqual(code, 0, result.get("failure"))
            battle, world = result["battle_input"], result["world_ui"]
            self.assertEqual(battle["groups"]["3"], [X_WING_SQUADRON, NEBULON], battle["log"])
            overlays = {(row["entity"], row["squadron"]): row for row in world["ability_overlays"]}
            self.assertEqual(overlays[(X_WING_SQUADRON, True)]["icon"], "i_sa_s_foil_mode.tga")
            self.assertEqual(overlays[(NEBULON, False)]["icon"], "i_sa_defend_mode.tga")
            # WU-43 / #983: native art in the authored lower-effect slot, entirely above
            # the 30-reference-pixel identity (the group text offset is -10 -6).
            groups = {(row["entity"], row["squadron"]): row for row in world["control_groups"]}
            scale = VIEWPORT[1] / 768
            group_centre = groups[(X_WING_SQUADRON, True)]["centre"]
            identity = (group_centre[0] + 10 * scale, group_centre[1] - 6 * scale)
            ability = overlays[(X_WING_SQUADRON, True)]
            self.assertAlmostEqual(ability["centre"][0], identity[0], delta=0.002)
            self.assertAlmostEqual(ability["centre"][1], identity[1] - 30 * scale, delta=0.002)
            self.assertEqual(ability["size"], [26 * scale, 26 * scale])
            self.assertLess(ability["centre"][1] + ability["size"][1] / 2, identity[1] - 15 * scale)
            numbers = {(row["entity"], row["squadron"]): row["number"] for row in world["control_groups"]}
            self.assertEqual(numbers[(X_WING_SQUADRON, True)], 3)
            self.assertEqual(numbers[(NEBULON, False)], 3)
            self.assertNotIn((X_WING_SQUADRON_3, True), numbers)
            self.assertTrue(result["live_session"]["headless_hashes_equal"])
            # WU-46 regression control: identical active battle and selection, with only
            # world ability art suppressed. Translucent identity pixels see the same world.
            code, identity_off = self._run(directory, "ability-art-control-off-lit", inputs, end_tick=210,
                                          env={"EAWR_WORLD_ABILITY_ART_CONTROL": "off"})
            self.assertEqual(code, 0, identity_off.get("failure"))
            self.assertEqual(identity_off["world_ui"]["ability_overlays"], [])
            self.assertTrue(identity_off["live_session"]["headless_hashes_equal"])
            self.assertEqual(identity_off["live_session"]["final_state_sha256"],
                             result["live_session"]["final_state_sha256"])
            self.assertCountEqual(identity_off["battle_input"]["selected"], battle["selected"])
            self.assertEqual(identity_off["world_ui"]["icon_rows"], world["icon_rows"])
            inactive_group = next(row for row in identity_off["world_ui"]["control_groups"]
                                  if row["entity"] == X_WING_SQUADRON and row["squadron"])
            self.assertEqual(inactive_group["centre"], group_centre)
            _, _, active_pixels = decode_png((directory / "ability-overlay-on-lit.png").read_bytes())
            _, _, inactive_pixels = decode_png((directory / "ability-art-control-off-lit.png").read_bytes())
            # Every fully covered identity pixel, including the group digit, remains identical.
            left, right = math.ceil(identity[0] - 15 * scale), math.floor(identity[0] + 15 * scale)
            top, bottom = math.ceil(identity[1] - 15 * scale), math.floor(identity[1] + 15 * scale)
            active_crop = [row[left:right] for row in active_pixels[top:bottom]]
            inactive_crop = [row[left:right] for row in inactive_pixels[top:bottom]]
            self.assertEqual(active_crop, inactive_crop, "active lower-effect art must preserve all squadron identity pixels")
            # The separately drawn ability pixels do change above that unchanged identity.
            ax, ay = ability["centre"]
            aw, ah = ability["size"]
            changed_above = sum(active_pixels[y][x] != inactive_pixels[y][x]
                                for y in range(math.ceil(ay - ah / 2), math.floor(ay + ah / 2))
                                for x in range(math.ceil(ax - aw / 2), math.floor(ax + aw / 2)))
            self.assertGreater(changed_above, 20, "active ability must draw visible pixels above the squadron identity")
            code, off = self._run(directory, "ability-overlay-off-lit", (*inputs,
                "--eawr-live-input", "215:click:icon=2",
                "--eawr-live-input", "225:key:W+shift",
                "--eawr-live-input", "235:click:unit=6",
                "--eawr-live-input", "245:key:O+shift"), end_tick=270)
            self.assertEqual(code, 0, off.get("failure"))
            self.assertFalse(any(row["entity"] in (X_WING_SQUADRON, NEBULON)
                                 for row in off["world_ui"]["ability_overlays"]), off["world_ui"])
            self.assertTrue(off["live_session"]["headless_hashes_equal"])


    def test_hunt_button_toggles(self):
        # WAB-50/55: the Empire's own squadron exposes a usable Hunt icon and untimed mark.
        empire_camera = CAMERA.parent / "coruscant-empire-live-session-camera.xml"
        selected = ("--eawr-live-player", "2", "--eawr-live-ai", "off",
                    "--eawr-lighting", "sh", "--eawr-environment", "map",
                    "--eawr-shadows", "on", "--eawr-live-input", f"20:click:icon={TIE_SQUADRON}")
        with tempfile.TemporaryDirectory(prefix="eawr-hunt-button-") as temporary:
            directory = pathlib.Path(temporary)
            code, ready = self._run(directory, "hunt-ready", selected, camera=empire_camera, end_tick=60)
            self.assertEqual(code, 0, ready.get("failure"))
            buttons = ready["hud"]["ability_buttons"]["buttons"]
            self.assertEqual(len(buttons), 1, buttons)
            self.assertEqual(buttons[0]["name"], "HUNT", buttons)
            self.assertFalse(buttons[0]["disabled"], buttons)
            self.assertFalse(buttons[0]["autofire"], buttons)
            self.assertEqual(buttons[0]["recharge"], 1, buttons)
            self.assertEqual(ready["hud"]["ability_buttons"]["textures_missing"], 0)
            code, active = self._run(directory, "hunt-active", (
                *selected, "--eawr-live-input", "30:click:ability=0"), camera=empire_camera, end_tick=60)
            self.assertEqual(code, 0, active.get("failure"))
            requests = active["live_session"]["ability_requests"]
            self.assertEqual((requests["issued"], requests["refused"]), (1, 0), requests)
            marks = active["hud"]["ability_buttons"]["marks"]
            self.assertTrue(marks, active["hud"]["ability_buttons"])
            self.assertTrue(all(mark["icon"] == buttons[0]["icon"] and mark["dial"] is None
                                and not mark["autofire"] for mark in marks), marks)
            code, stopped = self._run(directory, "hunt-stopped", (
                *selected, "--eawr-live-input", "30:click:ability=0", "--eawr-live-input", "50:key:H+shift"),
                camera=empire_camera, end_tick=60)
            self.assertEqual(code, 0, stopped.get("failure"))
            requests = stopped["live_session"]["ability_requests"]
            self.assertEqual((requests["issued"], requests["refused"]), (2, 0), requests)
            self.assertEqual(stopped["hud"]["ability_buttons"]["marks"], [])
            # WAB-51: both ordinary Stop inputs cancel Hunt without an ability command.
            for label, gesture in (("hunt-stop-button", "50:click:hud=stop"),
                                   ("hunt-stop-key", "50:key:S")):
                code, ordered = self._run(directory, label, (
                    *selected, "--eawr-live-input", "30:click:ability=0",
                    "--eawr-live-input", gesture), camera=empire_camera, end_tick=100)
                self.assertEqual(code, 0, ordered.get("failure"))
                requests = ordered["live_session"]["ability_requests"]
                self.assertEqual((requests["issued"], requests["refused"]), (1, 0), requests)
                self.assertEqual(ordered["battle_input"]["orders"], 1)
                self.assertIn("stop", ordered["battle_input"]["log"])
                self.assertEqual(ordered["live_session"]["rejected"], [])
                self.assertEqual(ordered["hud"]["ability_buttons"]["marks"], [])
                self.assertIs(ordered["live_session"]["headless_hashes_equal"], True)
            for result in (ready, active, stopped):
                self.assertIs(result["live_session"]["headless_hashes_equal"], True)

    def test_ability_buttons_draw_and_request(self):
        from tools.inventory.census_catalog import Catalog, abilities, tokens, value

        catalog = Catalog(os.environ["EAWR_EAW_GAME_ROOT"])

        def authored_autofire(card, name):
            obj = catalog.resolve(card["type"])
            # AB-15/60: ion is the team's own slot; other squadron slots use craft data.
            if card["squadron"]:
                source = (value(obj, "Create_Team_Type") or "Team") if name == "ION_CANNON_SHOT" else (
                    tokens(value(obj, "Squadron_Units"))[0])
                obj = catalog.resolve(source)
            matches = [ability for ability in abilities(obj)[0] if ability["type"] == name]
            self.assertEqual(len(matches), 1, (card, name))
            fields = {key.lower(): values for key, values in matches[0]["fields"].items()}
            supported = fields.get("supports_autofire", ["false"])[-1]
            # AB-45: the human owner's fresh-profile creation preference is enabled.
            return supported.strip().lower() in ("yes", "true", "1")

        # #454 (docs/behaviour/foc-ability-buttons.md): the box's selection gets one button per
        # ability group under its border, with the engine's icons; a left release requests the ability
        # and a right release autofire, FoC's default keys press the buttons. Since #76 the buttons read
        # the simulation's ability state (a cut ability, the Y-wing's ION_CANNON_SHOT, shows disabled:
        # space-abilities AB-03) and the requests become ability commands in the replay.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-abilities-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "abilities", boxed, end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            battle, drawn = shown["battle_input"], shown["hud"]["ability_buttons"]
            cards = battle["unit_cards"]
            groups = sorted({card["ability"] for card in cards["cards"] if card["ability"] != 0})
            self.assertEqual(drawn["components"], 24)
            self.assertEqual(len(drawn["buttons"]), len(groups), drawn)
            self.assertEqual(drawn["textures_missing"], 0, drawn)
            expected_marks = []
            for button in drawn["buttons"]:
                self.assertTrue(button["icon"].startswith("i_sa_"), button)
                # WAB-50: HUNT is ready alongside the power modes and the team ion shot.
                self.assertFalse(button["disabled"], button)
                members = [card for card in cards["cards"] if card["ability"] == button["ability"]]
                defaults = [authored_autofire(card, button["name"]) for card in members]
                self.assertTrue(defaults, button)
                self.assertEqual(button["autofire"], all(defaults), button)
                for card, enabled in zip(members, defaults):
                    if enabled:
                        expected_marks.append({"slot": card["slot"], "second": button["second"],
                                               "icon": button["icon"], "dial": None, "autofire": True})
                self.assertEqual(button["recharge"], 1)
                # AB-01: the button of a group spanning columns a..b is special_button_(a + b).
                columns = [card["slot"] // 2 for card in cards["cards"] if card["ability"] == button["ability"]]
                self.assertEqual(button["component"], min(columns) + max(columns) + (1 if button["second"] else 0))
            self.assertCountEqual(drawn["marks"], expected_marks)

            code, clicked = self._run(directory, "ability-click", (
                *boxed, "--eawr-live-input", "30:click:ability=0", "--eawr-live-input", "35:rclick:ability=0",
                "--eawr-live-input", "40:key:O+shift"), end_tick=60)
            self.assertEqual(code, 0, clicked.get("failure"))
            log = clicked["battle_input"]["log"]
            self.assertEqual(clicked["hud"]["ability_buttons"]["clicks"], 1)
            self.assertEqual(clicked["hud"]["ability_buttons"]["right_clicks"], 1)
            # #76: every request reaches the simulation's ability commands (a cut one is refused there).
            requests = clicked["live_session"]["ability_requests"]
            # Shift+O is DEFEND's key; the Nebulon-B's group has DEFEND in the M2 start, and switching
            # it on changes the battle.
            if any(button["name"] == "DEFEND" for button in drawn["buttons"]):
                self.assertEqual(clicked["battle_input"]["ability_bar"]["hotkeys"], 1, log)
                self.assertEqual(requests["issued"] + requests["refused"], 3, requests)
                self.assertNotEqual(clicked["live_session"]["final_state_sha256"],
                                    shown["live_session"]["final_state_sha256"])
            else:
                self.assertEqual(requests["issued"] + requests["refused"], 2, requests)
            self.assertIs(clicked["live_session"]["headless_hashes_equal"], True)
            self.assertEqual(clicked["battle_input"]["selected"], battle["selected"])

            code, demo = self._run(directory, "ability-demo", (*boxed, "--eawr-live-ability-demo", "on"), end_tick=60)
            self.assertEqual(code, 0, demo.get("failure"))
            staged = demo["hud"]["ability_buttons"]
            self.assertTrue(demo["battle_input"]["ability_bar"]["demo"])
            by_rank = staged["buttons"]
            self.assertTrue(any(button["disabled"] for button in by_rank), by_rank)
            self.assertTrue(any(button["autofire"] for button in by_rank), by_rank)
            self.assertTrue(any(button["recharge"] < 1 for button in by_rank), by_rank)
            self.assertGreater(staged["dials_drawn"], 0, staged)
            # #521 (AB-08): a mark that is drawn at all (autofire box, dial or icon) always carries
            # the ability's icon too, including the ready-on-autofire rank; none draws a bare box.
            self.assertTrue(staged["marks"], staged["marks"])
            self.assertTrue(all(mark["icon"] for mark in staged["marks"]), staged["marks"])
            self.assertEqual(staged["textures_missing"], 0, staged)
            self.assertEqual(demo["live_session"]["final_state_sha256"], shown["live_session"]["final_state_sha256"])


    def test_ability_buttons_clear_with_the_selection(self):
        # #534: the ability area mirrors the current selection (docs/behaviour/foc-ability-buttons.md
        # "Interface"); its buttons must not linger once the selection they belonged to is gone,
        # whichever way it went: a deselect click, a squadron clicked off, its last unit dying, or a
        # group thinned down to nothing. Checked on both the model (battle_input.ability_bar) and what
        # the HUD actually drew (hud.ability_buttons), so a stale button can't hide behind a stale model.
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-abilities-clear-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "abilities-shown", boxed, end_tick=60)
            self.assertEqual(code, 0, shown.get("failure"))
            cards = shown["battle_input"]["unit_cards"]["cards"]
            self.assertTrue(shown["battle_input"]["ability_bar"]["buttons"], shown["battle_input"]["ability_bar"])
            self.assertTrue(shown["hud"]["ability_buttons"]["buttons"], shown["hud"]["ability_buttons"])

            # A click on empty space deselects everything: no button is left over the empty panel.
            code, cleared = self._run(directory, "abilities-cleared", (
                *boxed, "--eawr-live-input", f"30:click:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, cleared.get("failure"))
            self.assertEqual(cleared["battle_input"]["selected"], [], cleared["battle_input"]["log"])
            self.assertEqual(cleared["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(cleared["hud"]["ability_buttons"]["buttons"], [])

            # #424: a squadron selected by its icon, then deselected the same way.
            code, squadron_cleared = self._run(directory, "abilities-squadron-cleared", (
                "--eawr-live-input", f"20:click:icon={Y_WING_SQUADRON}",
                "--eawr-live-input", f"30:click:@{DESTINATION[0]},{DESTINATION[1]},0"), end_tick=60)
            self.assertEqual(code, 0, squadron_cleared.get("failure"))
            self.assertEqual(squadron_cleared["battle_input"]["selected"], [], squadron_cleared["battle_input"]["log"])
            self.assertEqual(squadron_cleared["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(squadron_cleared["hud"]["ability_buttons"]["buttons"], [])

            # A group thinned to nothing: Shift+click off a squadron card drops exactly its members
            # (proven by test_unit_cards_show_and_change_the_selection); when it is the sole holder of
            # its ability, that ability's button must go with it, never lingering for the units left.
            squadron_card = next(card for card in cards if card["squadron"])
            self.assertNotEqual(squadron_card["ability"], 0, squadron_card)
            sole_holder = sum(1 for card in cards if card["ability"] == squadron_card["ability"]) == 1
            code, thinned = self._run(directory, "abilities-group-thinned", (
                *boxed, "--eawr-live-input", f"30:click:card={squadron_card['slot']}+shift"), end_tick=60)
            self.assertEqual(code, 0, thinned.get("failure"))
            self.assertTrue(thinned["battle_input"]["selected"], thinned["battle_input"]["log"])
            before, after = shown["battle_input"]["ability_bar"]["buttons"], thinned["battle_input"]["ability_bar"]["buttons"]
            if sole_holder:
                self.assertLess(len(after), len(before), (before, after))
            self.assertEqual(len(thinned["hud"]["ability_buttons"]["buttons"]), len(after))

            # The selection's only unit dying (HD-30 scripted damage, #81) clears the bar too. The
            # Rebel companies start stacked on the spawn point (P-1 picks the topmost contact there),
            # so clearing the stack around the Nebulon-B first (as test_selected_ship_bars_and_-
            # hovered_hardpoints does) is what makes the click land on it alone.
            clear_stack = (*SPREAD, *MC80_ASIDE, "--eawr-live-order", f"1:move:{Y_WING_SQUADRON}@-5500,4400,0")
            code, isolated = self._run(directory, "abilities-lone-nebulon", (
                *clear_stack, "--eawr-live-input", f"250:click:unit={NEBULON}"), end_tick=270)
            self.assertEqual(code, 0, isolated.get("failure"))
            self.assertEqual(isolated["battle_input"]["selected"], [NEBULON], isolated["battle_input"]["log"])
            nebulon_card = next(card for card in isolated["battle_input"]["unit_cards"]["cards"]
                                if NEBULON in card["members"])
            self.assertNotEqual(nebulon_card["ability"], 0, nebulon_card)
            self.assertTrue(isolated["battle_input"]["ability_bar"]["buttons"], isolated["battle_input"]["ability_bar"])
            self.assertTrue(isolated["hud"]["ability_buttons"]["buttons"], isolated["hud"]["ability_buttons"])

            code, died = self._run(directory, "abilities-death", (
                *clear_stack, "--eawr-live-input", f"250:click:unit={NEBULON}",
                "--eawr-live-order", f"255:damage:{NEBULON}@500000"), end_tick=270)
            self.assertEqual(code, 0, died.get("failure"))
            self.assertEqual(died["battle_input"]["selected"], [], died["battle_input"]["log"])
            self.assertEqual(died["battle_input"]["ability_bar"]["buttons"], [])
            self.assertEqual(died["hud"]["ability_buttons"]["buttons"], [])


    def test_card_click_uses_selection_changed_in_the_same_frame(self):
        width, height = VIEWPORT
        box = ((0.05 * width, 0.05 * height), (0.95 * width, 0.95 * height))
        boxed = ("--eawr-live-input", f"20:box:{screen(box[0])}/{screen(box[1])}")
        with tempfile.TemporaryDirectory(prefix="eawr-battle-cards-same-frame-") as temporary:
            directory = pathlib.Path(temporary)
            code, shown = self._run(directory, "boxed", boxed, end_tick=40)
            self.assertEqual(code, 0, shown.get("failure"))
            cards = shown["battle_input"]["unit_cards"]["cards"]
            self.assertGreater(len(cards), 1, cards)
            ability = next(card["ability"] for card in cards if card["slot"] == 0)
            expected = sorted(member for card in cards if card["ability"] == ability
                              for member in card["members"])
            self.assertNotEqual(expected, sorted(shown["battle_input"]["selected"]))

            # Both events are replayed after the frame's initial card refresh. The click must
            # resolve slot 0 against the box's new selection, then publish those cards to the HUD.
            code, picked = self._run(directory, "same-frame", (
                *boxed, "--eawr-live-input", "20:click:card=0"), end_tick=40)
            self.assertEqual(code, 0, picked.get("failure"))
            battle = picked["battle_input"]
            self.assertEqual(battle["scripted_fired"], 2, battle["log"])
            self.assertEqual(battle["unit_cards"]["clicks"], 1, battle["log"])
            self.assertEqual(sorted(battle["selected"]), expected, battle["log"])
            self.assertEqual(picked["hud"]["unit_cards"]["clicks"], 1)
            self.assertEqual(battle["events"], 2, battle["log"])
            self.assertEqual(sorted(member for card in battle["unit_cards"]["cards"]
                                    for member in card["members"]), expected)
