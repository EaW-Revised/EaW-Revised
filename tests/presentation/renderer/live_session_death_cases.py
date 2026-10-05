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


class LiveSessionDeathCases:
    def test_heavy_melee_retains_burst_explosion_births_lit(self):
        # PS-18/PS-19: real death-clone bursts must fit the authored bound.
        with tempfile.TemporaryDirectory(prefix="eawr-heavy-burst-capacity-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "heavy-bursts", (
                "--eawr-live-melee", "l", "--eawr-live-melee-seed", "601",
                "--eawr-live-workers", "4", "--eawr-live-step", "2",
                "--eawr-live-ticks", "1800", "--eawr-live-reveal", "on",
                "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                "--eawr-hud", "off", "--eawr-audio", "off"),
                session=("--eawr-live-session", "melee"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            self.assertEqual(result["live_session"]["units"], 580)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            clones = emitters["death_clones"]
            self.assertEqual(clones["failed"], {})
            self.assertEqual(clones["not_run"], {})
            self.assertGreater(clones["started"].get("p_imperial_explosion_big00", 0), 0,
                               "the regression must exercise the formerly capped explosion")
            self.assertGreater(clones["max_particles"], 256)
            self.assertEqual(clones["dropped_at_capacity"], 0, clones)

    def test_destroyed_corvette_plays_its_death_clone(self):
        # #81 (docs/behaviour/unit-animation.md UA-07, UA-08): scripted damage destroys the
        # corvette at tick 15; its Corellian_Corvette_Death_Clone takes its place and plays
        # rv_corvette_d_die_00 on the presentation clock. The hashes stay the headless ones.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-death-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "death", (
                "--eawr-live-order", f"15:damage:{CORVETTE}@100000", "--eawr-live-capture-ticks", "0,30,60,120",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            clones = {row["unit"]: row for row in live["death_clones"]}
            self.assertEqual(clones[CORVETTE]["clone"], "Corellian_Corvette_Death_Clone")
            self.assertEqual(clones[CORVETTE]["clip"], "data/art/models/rv_corvette_d_die_00.ala")
            self.assertEqual([row["unit"] for row in live["death_clones_shown"]], [CORVETTE])
            # Death_Persistence_Duration -1: the clone holds its last frame and never leaves.
            self.assertEqual((clones[CORVETTE]["clip_type"], clones[CORVETTE]["persistence_ticks"]), ("DIE", None))
            self.assertEqual(live["death_clones_retired"], [])
            corvette = next(row for row in live["unit_clips"] if row["type"] == "Corellian_Corvette")
            self.assertEqual(sum(corvette["clips"].values()), 0)
            self.assertEqual(corvette["clone_clips"]["DIE"], 1)
            units = result["populate"]["live_units"]
            self.assertEqual(units["clips"]["Corellian_Corvette_Death_Clone data/art/models/rv_corvette_d_die_00.ala"],
                             "bound")
            self.assertGreater(units["clip_poses"], 0)
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            frames = [decode_png((directory / f"death_t{tick:04d}.png").read_bytes()) for tick in (30, 60, 120)]
            for before, after in zip(frames, frames[1:]):
                self.assertNotEqual(before[2], after[2])


    def test_destroyed_hardpoints_throw_their_breakoff_props(self):
        # #391 (docs/behaviour/battle-presentation.md BP-30 to BP-36): scripted damage destroys
        # the Nebulon-B's five hardpoints at ticks 15 to 75. The four weapons throw their
        # Hardpoint_Breakoff_Nebulon_Weapon_* props with their fires; the engines throw none.
        # Every prop lives 15 to 25 whole seconds, then leaves with its Medium_Explosion_Space.
        # Nothing reaches the simulation: the hashes stay the headless ones.
        orders = []
        for index, tick in enumerate((15, 30, 45, 60, 75)):
            orders += ["--eawr-live-order", f"{tick}:damage:{NEBULON}@100000,{index}"]
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff", (
                *orders, "--eawr-live-step", "15", "--eawr-live-capture-ticks", "15,90,300",
                "--eawr-live-ticks", "900", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            nebulon = [row for row in props["prepared"] if row["unit"] == NEBULON]
            self.assertEqual([(row["hardpoint"], row["prop"], row["status"]) for row in nebulon],
                             [(index, f"Hardpoint_Breakoff_Nebulon_Weapon_{side}", "ready")
                              for index, side in enumerate(("FL", "FR", "BL", "BR"))])
            self.assertTrue(all(row["fire"] == "Space_Debris_Fire_Large" for row in nebulon))
            self.assertTrue(all(row["explosion"] == "Medium_Explosion_Space" for row in nebulon))
            spawned = [row for row in props["spawned"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in spawned], [0, 1, 2, 3])
            # Each at the tick its hardpoint died: the order's tick, or the one after it.
            for row, ordered in zip(spawned, (15, 30, 45, 60)):
                self.assertIn(row["tick"] - ordered, (0, 1), row)
            for row in spawned:
                self.assertEqual(row["lifetime_frames"] % 30, 0)
                self.assertTrue(450 <= row["lifetime_frames"] <= 750, row)
            # By tick 900 every prop has run out and left with its explosion.
            self.assertEqual(props["live"], 0)
            self.assertEqual(sorted((row["unit"], row["hardpoint"]) for row in props["expired"]),
                             [(NEBULON, index) for index in range(4)])
            self.assertEqual(props["effects_started"].get("fire:Space_Debris_Fire_Large"), 4)
            self.assertEqual(props["effects_started"].get("death:Medium_Explosion_Space"), 4)
            self.assertEqual(props["effects_failed"], {})
            units = result["populate"]["live_units"]
            self.assertEqual(units["breakoffs_drawn"], len([row for row in props["prepared"] if row["status"] == "ready"]))
            # Expired props are hidden, not retired: a repaired hardpoint can throw its prop again.
            self.assertEqual(units["ships_retired"], 0)
            for name in ("breakoff_t0015.png", "breakoff_t0090.png", "breakoff_t0300.png"):
                self.assertIn(name, result["captures"])


    def test_breakoff_props_after_a_first_frame_stall(self):
        # #401 review 1 and 2: the first frame presents tick 200 and reaches the Nebulon-B's
        # hardpoint deaths of about ticks 15 and 150 at once. Tick 15's snapshot has left the
        # 64-tick history, so what the player saw then is unknown and that prop is not thrown
        # (never this frame's visibility instead); tick 150's is kept and its prop is thrown. The
        # props' effect clock starts at the oldest reached event, not at tick 200, so the fire
        # born at tick 149 or 150 keeps its age.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-stall-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff_stall", (
                "--eawr-live-order", f"15:damage:{NEBULON}@100000,0",
                "--eawr-live-order", f"150:damage:{NEBULON}@100000,1",
                "--eawr-live-stall", "start:200", "--eawr-live-step", "15", "--eawr-live-ticks", "300",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            spawned = [row for row in props["spawned"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in spawned], [1], props["spawned"])
            self.assertIn(spawned[0]["tick"] - 150, (0, 1), spawned[0])
            self.assertGreaterEqual(props["unknown"], 1)
            self.assertLessEqual(props["clock_start"], spawned[0]["tick"] - 1)
            self.assertGreaterEqual(props["effects_started"].get("fire:Space_Debris_Fire_Large", 0), 1)


    def test_breakoff_explosion_ended_during_a_stall_is_skipped(self):
        # #401 review 2: the Nebulon-B's front-left prop is thrown at about tick 15 and dies 15 to
        # 25 s later (by presented tick 765), inside a stall that jumps from tick 30 to 945. Its
        # 30-frame Medium_Explosion_Space ended long before that frame: it is not started.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-breakoff-late-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "breakoff_late", (
                "--eawr-live-order", f"15:damage:{NEBULON}@100000,0",
                "--eawr-live-stall", "30:900", "--eawr-live-step", "15", "--eawr-live-ticks", "1000",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            props = result["breakoff_props"]
            self.assertIsNone(props["error"])
            self.assertEqual([row["hardpoint"] for row in props["spawned"] if row["unit"] == NEBULON], [0])
            expired = [row for row in props["expired"] if row["unit"] == NEBULON]
            self.assertEqual([row["hardpoint"] for row in expired], [0])
            self.assertLess(expired[0]["death_tick"], 900)
            self.assertGreaterEqual(props["effects_skipped"].get("death:Medium_Explosion_Space", 0), 1)
            if all(row["death_tick"] < 900 for row in props["expired"]):
                self.assertIsNone(props["effects_started"].get("death:Medium_Explosion_Space"))


    def test_death_clones_without_a_clip_and_after_their_fade(self):
        # #363 review 2 and 3 (UA-08, DeathBehavior): with a death type the clone models lack
        # (CRUSHED), the corvette clone (Remove_Upon_Death) is removed at once and the Rebel
        # station's clone (no Remove_Upon_Death) keeps its pose; with a 1 s persistence it fades
        # 1 s + 0.25 s after it appeared and leaves: its ship is retired, its uploads released.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-clone-rules-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "rules", (
                "--eawr-live-order", f"15:damage:{STATION}@1000000", "--eawr-live-order", f"15:damage:{CORVETTE}@100000",
                "--eawr-live-death-anim", "CRUSHED", "--eawr-live-death-persistence", "1",
                "--eawr-live-capture-ticks", "0,30,90", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            # #77 (space-victory VT-05): the Rebel station's loss is the local player's defeat.
            self.assertEqual(live["outcome"], {
                "condition": "enemy_starbase_destroyed", "winner": 2, "winner_team": 1, "decided_tick": 15,
                "deciding_unit": STATION, "end_tick": 225, "local_result": "defeat"})
            clones = {row["unit"]: row for row in live["death_clones"]}
            self.assertEqual((clones[CORVETTE]["clip_type"], clones[CORVETTE]["clip"], clones[CORVETTE]["remove_upon_death"]),
                             ("CRUSHED", "", True))
            self.assertIn("removed at once", clones[CORVETTE]["status"])
            self.assertEqual((clones[STATION]["clone"], clones[STATION]["clip"], clones[STATION]["remove_upon_death"]),
                             ("Rebel_Star_Base_1_Death_Clone", "", False))
            self.assertIn("keeps its pose", clones[STATION]["status"])
            self.assertEqual(live["death_clones_shown"], [])
            self.assertEqual([(row["unit"], row["reason"], row["clone_tick"]) for row in live["death_clones_retired"]],
                             [(STATION, "faded out", 38)])
            units = result["populate"]["live_units"]
            # WPR-52: both factions' five station levels have preloaded clones;
            # the capital ships' clones are not composed for this missing clip.
            self.assertEqual(units["clones_drawn"], 10)
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            self.assertEqual(units["ships_retired"], 1)
            self.assertGreater(units["released_assets"], 0)
            # The corvette (in the camera's view; the station is not) is gone at tick 30 with no clone.
            frames = [decode_png((directory / f"rules_t{tick:04d}.png").read_bytes()) for tick in (0, 30)]
            self.assertNotEqual(frames[0][2], frames[1][2])

            # The corvette clone with its DIE clip and no persistence leaves once the clip has
            # ended and its fade has run out.
            code, result = self._run(directory, "fade", (
                "--eawr-live-order", f"15:damage:{CORVETTE}@100000", "--eawr-live-death-persistence", "0",
                "--eawr-live-ticks", "600", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["death_clones_shown"], [])
            self.assertEqual([(row["unit"], row["reason"]) for row in live["death_clones_retired"]],
                             [(CORVETTE, "faded out")])
            self.assertIsNone(live["outcome"], "a corvette's loss decides nothing (VT-02)")
            self.assertGreater(result["populate"]["live_units"]["clip_poses"], 0)
            self.assertEqual(result["populate"]["live_units"]["ships_retired"], 1)


    def test_unit_emitters_follow_engines_and_destroyed_hardpoints(self):
        # #394 (battle-presentation BP-40 to BP-43): the live units run their own engine emitters,
        # never the turbo ones hidden at creation, and a destroyed hardpoint starts its damage
        # emitters. Destroying the Nebulon-B's engines stops its engine emitter and starts the
        # two damage emitters below HP_E_EmitDamage. Presentation only: the hashes stay headless.
        with tempfile.TemporaryDirectory(prefix="eawr-live-emitters-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "emitters", (
                "--eawr-live-ticks", "150",
                "--eawr-live-order", f"15:move:{NEBULON}@-4400,5400,0",
                "--eawr-live-order", f"60:damage:{NEBULON}@5000,{NEBULON_FL}",
                "--eawr-live-order", f"105:damage:{NEBULON}@5000,{NEBULON_ENGINES}"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            self.assertEqual(emitters["start_failed"], {})
            started = emitters["started"]
            self.assertEqual(started.get("pe_nebulonengines"), 1, started)
            # FL: one damage emitter; the engines: two more, and the engine emitter stops.
            self.assertEqual(started.get("p_hp_stardestroyer_damage"), 3, started)
            self.assertEqual(emitters["stopped"].get("hardpoint_state"), 1, emitters["stopped"])
            self.assertGreater(emitters["max_particles"], 0)
            # The Empire player's view: the Tartan runs its "pe" engines, never its "pte" ones.
            code, result = self._run(directory, "tartan", ("--eawr-live-ticks", "30", "--eawr-live-player", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            emitters = result["unit_emitters"]
            started = emitters["started"]
            self.assertEqual(started.get("pe_tartanengine_sml"), 1, started)
            self.assertEqual(started.get("pe_tartanengine_lrg"), 1, started)
            self.assertFalse(any(name.lower().startswith("pte") for name in started), started)
            self.assertTrue(any(key.lower().startswith("pte_tartanengine") and "hidden at creation" in key
                                for key in emitters["not_admitted"]), emitters["not_admitted"])


    def test_death_clone_pieces_burn_until_they_vanish(self):
        # #421 (battle-presentation BP-47 to BP-50): the corvette's death clone runs its own
        # proxies on its DIE clip. Its four p_rebelsmokedeath and its p_debris01 start with the
        # clone and stop emitting (drain) when their pieces vanish; each p_explosion_big00 runs
        # only in the last frame its piece shows (clip frames 74, 80 and 83). Presentation only.
        with tempfile.TemporaryDirectory(prefix="eawr-live-clone-fire-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "clonefire", (
                "--eawr-live-ticks", "150", "--eawr-live-order", f"15:damage:{CORVETTE}@100000"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            emitters = result["unit_emitters"]
            self.assertIsNone(emitters["failure"])
            clones = emitters["death_clones"]
            self.assertEqual(clones["ships"], 1, clones)
            self.assertEqual(clones["failed"], {})
            self.assertEqual(clones["not_run"], {})
            expected = {"p_rebelsmokedeath": 4, "p_debris01": 1, "p_explosion_big00": 3}
            self.assertEqual(clones["started"], expected)
            # By tick 150 the clip (86 frames) has ended with every piece hidden.
            self.assertEqual(clones["hidden"], expected)
            self.assertGreater(clones["max_particles"], 0)
            rows = clones["start_log"]
            first = min(row["born"] for row in rows)
            self.assertTrue(all(row["born"] == first for row in rows if row["proxy"] != "p_explosion_big00"), rows)
            self.assertEqual(sorted(row["born"] - first for row in rows if row["proxy"] == "p_explosion_big00"),
                             [74, 80, 83], rows)
            # The Rebel station's clone: its fire billows, electrical damage and explosion chains
            # emit from their pieces' meshes (EnhancedMesh). In the first 45 clip frames only the
            # proxies its clip shows from frame 0 have started.
            code, result = self._run(directory, "stationfire", (
                "--eawr-live-ticks", "60", "--eawr-live-order", f"15:damage:{STATION}@1000000"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            clones = result["unit_emitters"]["death_clones"]
            self.assertEqual((clones["ships"], clones["failed"], clones["not_run"]), (1, {}, {}), clones)
            self.assertEqual(clones["started"], {"p_fire_billow2": 4, "p_fire_billow5": 1, "p_damage_elec04": 4,
                                                 "p_explosion_chain": 1, "p_explosion_chain3": 5}, clones)
            # #429 review 2 (BP-48): a proxy whose piece shows again is reset, as the debug
            # build resets the group (BP-48): its old drain goes.
            self.assertEqual(clones["reappearance"], "reset", clones)


    def test_death_clone_emitters_across_a_stall_past_the_fade(self):
        # #429 review 1: a stall that jumps from before the corvette's death (tick 15) past its
        # DIE clip and fade (no persistence) still runs the clone's samples at their own ticks:
        # its smoke, debris and last-frame explosions start where a paced run starts them, and
        # the clone's ship is retired only after those samples ran.
        expected = {"p_rebelsmokedeath": 4, "p_debris01": 1, "p_explosion_big00": 3}
        with tempfile.TemporaryDirectory(prefix="eawr-live-clone-stall-") as temporary:
            directory = pathlib.Path(temporary)
            runs = {}
            for name, extra in (("paced", ()), ("stalled", ("--eawr-live-stall", "10:120"))):
                code, result = self._run(directory, name, (
                    "--eawr-live-ticks", "150", "--eawr-live-order", f"15:damage:{CORVETTE}@100000",
                    "--eawr-live-death-persistence", "0", *extra))
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertIs(live["headless_hashes_equal"], True)
                self.assertEqual([(row["unit"], row["reason"]) for row in live["death_clones_retired"]],
                                 [(CORVETTE, "faded out")])
                self.assertEqual(result["populate"]["live_units"]["ships_retired"], 1)
                emitters = result["unit_emitters"]
                self.assertIsNone(emitters["failure"])
                clones = emitters["death_clones"]
                self.assertEqual((clones["ships"], clones["failed"], clones["not_run"]), (1, {}, {}), clones)
                self.assertEqual(clones["started"], expected, (name, clones))
                self.assertEqual(clones["hidden"], expected, (name, clones))
                runs[name] = emitters
            # The stalled frame presents 130.5: every clone sample before it is caught up. The
            # oldest have left the snapshot history, so the ships skip them (least visible,
            # #406); the clone stands at its unit's last drawn pose and runs them all.
            self.assertGreater(runs["stalled"]["caught_up"], 100)
            self.assertGreater(runs["stalled"]["unknown_samples"], 0)
            births = {name: sorted((row["proxy"], row["born"]) for row in emitters["death_clones"]["start_log"])
                      for name, emitters in runs.items()}
            self.assertEqual(births["stalled"], births["paced"])


    def test_killed_fighters_spin_away_and_explode(self):
        # #447 (docs/behaviour/space-fighter-deaths.md SP-02 to SP-08): under the M2 seed the
        # X-wing 36 hit by the order of tick 150 (it dies in tick 151) spins away: its model flies
        # on and rolls for 60 ticks, then explodes and leaves; the Y-wing 46 (order 152, dies in
        # 153) only explodes. (The MC80 of #537 is unit 7, so craft IDs are one higher than before.)
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-spin-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "spin", (
                "--eawr-live-order", "150:damage:36@1000000", "--eawr-live-order", "152:damage:46@1000000",
                "--eawr-live-follow", "36", "--eawr-live-ticks", "230", "--eawr-live-step", "2",
                "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["spin_away"]["spins"], [{"unit": 36, "started": 151, "ended": 211}])
            self.assertEqual((live["spin_away"]["drawn_max"], live["spin_away"]["ships_max"]), (1, 1))
            spawned = result["battle_effects"]["spawned"]
            self.assertEqual(spawned.get("spin_away:Small_Explosion_Space"), 1)
            self.assertEqual(spawned.get("death:Small_Explosion_Space"), 2)


    def test_fighter_spin_away_in_a_seeded_dogfight(self):
        # #447: the Acclamator leads its TIE escort into the Rebel squadrons; under the M2 seed
        # some of the craft shot down in the dogfight spin away and explode 60 ticks later.
        # 7500 ticks, not 5400 (#465 review): #465's lead and out-of-combat fire against fighters
        # shifts which tick each craft dies on, so the three kills within the first 5400 ticks
        # draw differently than before; the fight keeps producing kills past that window and the
        # first one that spins away lands at tick 5589.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-dogfight-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "dogfight", (
                "--eawr-live-order", "2:move:11@-4300,3900,0", "--eawr-live-ticks", "7500",
                "--eawr-live-step", "30", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            spins = live["spin_away"]["spins"]
            self.assertGreaterEqual(len(spins), 1)
            # 60 ticks, or 60 + k for a craft killed at a roll of exactly -20k degrees, whose path
            # rebuilds k frames in (SP-04, SC-04; unit 81 here spins for 62).
            for spin in spins:
                self.assertIn(spin["ended"] - spin["started"], range(60, 70), spin)
            self.assertGreaterEqual(live["spin_away"]["drawn_max"], 1)
