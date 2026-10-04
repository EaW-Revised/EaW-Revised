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


class LiveSessionReplayCases:
    def test_unicode_camera_bindings_and_live_file_paths(self):
        with tempfile.TemporaryDirectory(prefix="eawr live caf\u00e9 ") as temporary:
            directory = pathlib.Path(temporary)
            camera = directory / "camera caf\u00e9.xml"
            shutil.copy2(CAMERA, camera)
            shutil.copy2(CAMERA.parent / "space-live-camera-bindings.json", directory)
            replay = directory / "record caf\u00e9.eawr-replay"
            hashes = directory / "live caf\u00e9.hashes.csv"
            font_cache = directory / "fonts"
            # Staged rig checkouts contain no ignored out/fonts; extract from this host's install.
            extracted = subprocess.run(
                [sys.executable, str(ROOT / "tools/fonts/extract_eaw_fonts.py"),
                 "--game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--out", str(font_cache)],
                text=True, capture_output=True, check=False)
            self.assertEqual(extracted.returncode, 0, extracted.stdout + extracted.stderr)
            common = ("--eawr-live-ticks", "8", "--eawr-live-step", "1", "--eawr-audio", "off",
                      "--eawr-font-cache", str(font_cache))
            code, recorded = self._run(directory, "record", (
                *common, "--eawr-live-hashes", str(hashes), "--eawr-live-replay-out", str(replay)), camera=camera)
            self.assertEqual(code, 0, recorded.get("failure"))
            self.assertTrue(hashes.is_file())
            self.assertTrue(replay.is_file())
            self.assertTrue((directory / "record.png").is_file())
            self.assertEqual(recorded["hud"]["font_cache"]["directory"], font_cache.as_posix())
            self.assertEqual(recorded["map_camera"]["overview_clicks"]["override"]["file"], camera.name)
            code, replayed = self._run(directory, "replay", (
                *common, "--eawr-live-replay", str(replay)), camera=camera,
                session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, replayed.get("failure"))
            self.assertEqual(replayed["live_session"]["replay"], replay.as_posix())
            self.assertTrue(replayed["live_session"]["headless_hashes_equal"])
            self.assertEqual(replayed["live_session"]["final_state_sha256"],
                             recorded["live_session"]["final_state_sha256"])

            name = "\u6218\u6597"
            code, captured = self._run(directory, name, (
                *common, "--eawr-live-capture-ticks", "2,4"), camera=camera)
            self.assertEqual(code, 0, captured.get("failure"))
            for filename in (f"{name}.png", f"{name}_t0002.png", f"{name}_t0004.png"):
                image = directory / filename
                self.assertTrue(image.is_file(), filename)
                self.assertGreater(image.stat().st_size, 0)
            self.assertEqual([file for file in captured["captures"] if file != "configured"],
                             [f"{name}_t0002.png", f"{name}_t0004.png"])

            missing = directory / "missing caf\u00e9.fog"
            code, refused = self._run(directory, "missing-fog", (
                "--eawr-fog-grid", str(missing), "--eawr-fog-sha256", "0" * 64,
                "--eawr-fog-team", "1", "--eawr-fog-revision", "0", "--eawr-fog-tick", "0"),
                session=(), camera=camera)
            self.assertEqual(code, 2)
            self.assertEqual(refused["failure"], f"cannot read fog grid: {missing.as_posix()}")


    def test_skirmish_loads_selected_faction_and_fleet_beyond_m2(self):
        # SC-01: default forces and fleet choices extend the unit tables at startup.
        # These types are absent from the pinned M2 load, so this catches silently
        # retaining that whitelist after accepting different lobby choices.
        with tempfile.TemporaryDirectory(prefix="eawr-selected-skirmish-") as temporary:
            code, result = self._run(pathlib.Path(temporary), "selected", (
                "--eawr-skirmish-players", "3,4",
                "--eawr-skirmish-slot", "3:Underworld:1:human",
                "--eawr-skirmish-slot", "4:Empire:0:ai",
                "--eawr-skirmish-fleet", "3:none",
                "--eawr-skirmish-fleet", "4:Star_Destroyer",
                "--eawr-skirmish-seed", "908",
                "--eawr-live-ai", "off", "--eawr-audio", "off",
                "--eawr-live-step", "1", "--eawr-live-ticks", "8",
                "--eawr-map-timed-frames", "8"),
                session=("--eawr-live-session", "skirmish"), camera=None,
                map_name="data/art/maps/_mp_space_polus.ted")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["start_map"], "data/art/maps/_mp_space_polus.ted")
            self.assertEqual(live["start_seed"], 908)
            self.assertEqual(live["local_player"], 3)
            spawns = {row["player"]: row["record"] for row in live["start_markers"]
                      if row["use"] == "spawn"}
            self.assertEqual(set(spawns), {3, 4})
            for player, unit_type in ((3, "starviper_squadron"), (4, "star_destroyer")):
                units = [row for row in live["start_fleet"]
                         if row["player"] == player and row["type"].lower() == unit_type]
                self.assertTrue(units, live["start_fleet"])
                self.assertTrue(all(row["record"] == spawns[player] for row in units))
            self.assertTrue(live["headless_hashes_equal"])


    def test_corvette_moves_and_turns_with_equal_hashes(self):
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-") as temporary:
            directory = pathlib.Path(temporary)
            ticks = ",".join(str(tick) for tick in CAPTURE_TICKS)
            code, result = self._run(directory, "live", (
                *ORDERS, "--eawr-live-capture-ticks", ticks, "--eawr-live-workers", "4",
                "--eawr-live-hashes", str(directory / "live.hashes.csv"),
                "--eawr-live-replay-out", str(directory / "live.eawr-replay")))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fixture"], live["pacing"], live["workers"], live["local_player"]),
                             ("m2", "driven", 4, 1))
            self.assertEqual(live["rejected"], [])
            self.assertGreaterEqual(live["completed_ticks"], CAPTURE_TICKS[-1])
            # The viewer-attached run equals a headless run of its recorded command stream.
            self.assertIs(live["headless_hashes_equal"], True)
            rows = (directory / "live.hashes.csv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(rows[0], "tick,sha256")
            self.assertEqual(len(rows) - 1, live["completed_ticks"])
            self.assertEqual(rows[-1].split(",")[1], live["final_state_sha256"])
            self.assertTrue((directory / "live.eawr-replay").is_file())
            # Own units are drawn; the Empire fleet and the unseen map objects are hidden.
            self.assertGreater(live["visible_units"], 0)
            self.assertGreater(live["hidden_units"], 0)
            units = result["populate"]["live_units"]
            self.assertEqual(units["session_records_skipped"], live["session_records"])
            # A squadron company is its team container and has no model (#75). Its craft are start
            # units: the X-wings draw; the Y-wing and TIE interceptor models do not compose through
            # the placed-ship path yet (fidelity list, #80). The laser pads draw since #80.
            self.assertTrue(all("Squadron" in item or "(Y-Wing)" in item or "(TIE_Interceptor)" in item
                                for item in units["not_drawn"]), units["not_drawn"])
            self.assertFalse(any("(X-Wing)" in item for item in units["not_drawn"]), units["not_drawn"])
            self.assertEqual(units["drawn"], live["units"] - len(units["not_drawn"]))
            # The corvette leaves the stack and turns: consecutive captures differ.
            frames = []
            for tick in CAPTURE_TICKS:
                capture = directory / f"live_t{tick:04d}.png"
                self.assertIn(capture.name, result["captures"])
                frames.append(decode_png(capture.read_bytes()))
            for before, after in zip(frames, frames[1:]):
                self.assertNotEqual(before[2], after[2])

            # The same orders with one worker end on the same state.
            code, single = self._run(directory, "single", (
                *ORDERS, "--eawr-live-ticks", str(CAPTURE_TICKS[-1]), "--eawr-live-workers", "1"))
            self.assertEqual(code, 0, single.get("failure"))
            self.assertEqual(single["live_session"]["completed_ticks"], live["completed_ticks"])
            self.assertEqual(single["live_session"]["final_state_sha256"], live["final_state_sha256"])


    def test_perf_overlay_reports_fps_frame_and_tick_cost(self):
        # #558: `--eawr-perf-overlay on` shows the overlay from the first frame; its report gives the
        # FPS, the frame time, the simulation's cost per tick and the overlay's own cost. The overlay
        # reads timers only, so the tick hashes equal a run without it, and the F3 key toggles it.
        # #972: cap presentation at 60 FPS. The overlay rebuilds at 20 Hz, so an uncapped run
        # amortizes fewer redraws over each shown frame and can conceal its CPU cost.
        engine_args = ("--max-fps", "60")
        with tempfile.TemporaryDirectory(prefix="eawr-perf-overlay-") as temporary:
            directory = pathlib.Path(temporary)
            common = (*ORDERS, "--eawr-live-ticks", "240", "--eawr-live-workers", "2")
            code, plain = self._run(directory, "plain", (
                *common, "--eawr-live-hashes", str(directory / "plain.hashes.csv")), engine_args=engine_args)
            self.assertEqual(code, 0, plain.get("failure"))
            self.assertIs(plain["perf_overlay"]["shown"], False, "off by default")
            self.assertEqual(plain["perf_overlay"]["key"], "F3")

            code, shown = self._run(directory, "shown", (
                *common, "--eawr-perf-overlay", "on", "--eawr-live-hashes", str(directory / "shown.hashes.csv")),
                engine_args=engine_args)
            self.assertEqual(code, 0, shown.get("failure"))
            overlay = shown["perf_overlay"]
            self.assertIs(overlay["shown"], True)
            self.assertGreater(overlay["frames"], 100)
            self.assertGreater(overlay["fps"], 0.0)
            self.assertGreaterEqual(overlay["fps"], 55.0, "the cost comparison needs the 60 FPS cadence")
            self.assertLessEqual(overlay["fps"], 62.0, "the presentation cap must apply")
            self.assertGreater(overlay["frame_ms"], 0.0)
            self.assertGreaterEqual(overlay["frame_ms_worst"], overlay["frame_ms_avg"])
            self.assertEqual(overlay["tick_source"], "live_session")
            self.assertGreater(overlay["ticks"], 0)
            self.assertGreater(overlay["tick_ms"], 0.0)
            self.assertGreaterEqual(overlay["tick_ms_worst"], overlay["tick_ms_avg"])
            # #957: the scripted M2 tick reports AI and Lua separately from the remaining step.
            self.assertEqual([phase["name"] for phase in overlay["tick_phases"]], ["step", "ai", "lua", "fog"])
            self.assertIsNotNone(overlay["visible_units"])
            self.assertGreater(overlay["own_cost_ms"]["samples"], 100)
            # #972: five exclusive 60 FPS runs per revision on this profile yielded mean/p95
            # 0.1104/0.162 ms before #963, 0.1270/0.140 after it and 0.1146/0.137 at 70d01a4b.
            # The overlay code is unchanged and the baseline already misses 0.100 ms. Allow
            # 0.038 ms above its observed p95 here; other and unprofiled hosts retain 0.100 ms.
            # This is overlay CPU wall time, including draw-command rebuilding, not GPU time.
            gpu_profile = os.environ.get("EAWR_GPU_PROFILE", "")
            own_budget_ms = {"nvidia-gtx970": 0.200}.get(gpu_profile, 0.100)
            self.assertLess(overlay["own_cost_ms"]["avg"], own_budget_ms,
                            {"gpu_profile": gpu_profile, "budget_ms": own_budget_ms, **overlay["own_cost_ms"]})
            self.assertGreater(overlay["rect"][2], 0)
            # A timer read never enters a hash.
            self.assertIs(shown["live_session"]["headless_hashes_equal"], True)
            plain_rows = (directory / "plain.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            shown_rows = (directory / "shown.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            common_rows = min(len(plain_rows), len(shown_rows))
            self.assertGreaterEqual(common_rows, 240)
            self.assertEqual(plain_rows[:common_rows], shown_rows[:common_rows])

            # The key: F3 shows it, a second F3 hides it again, and a third shows it once more.
            code, keyed = self._run(directory, "keyed", (
                *common, "--eawr-live-input", "f20:key:F3", "--eawr-live-input", "f60:key:F3",
                "--eawr-live-input", "f80:key:F3"), engine_args=engine_args)
            self.assertEqual(code, 0, keyed.get("failure"))
            self.assertEqual((keyed["perf_overlay"]["shown"], keyed["perf_overlay"]["toggles"]), (True, 3))
            self.assertGreater(keyed["perf_overlay"]["frames"], 10)
            code, bad = self._run(directory, "bad", ("--eawr-perf-overlay", "maybe"), engine_args=engine_args)
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-perf-overlay expects on or off", bad["failure"])


    def test_reveal_draws_the_hidden_empire_fleet(self):
        # #80: --eawr-live-reveal on now works with --eawr-live-session m2 too (a viewer debug
        # aid), not only replay. The Empire station (unit 7) sits behind the Rebel local
        # player's fog at the start; revealed, it draws (and lists in live_session.hostile_units)
        # while the simulation, the AI's commands among them, is unaffected: the two runs'
        # per-tick hashes are identical.
        with tempfile.TemporaryDirectory(prefix="eawr-live-reveal-") as temporary:
            directory = pathlib.Path(temporary)
            code, hidden = self._run(directory, "hidden", (
                "--eawr-live-ticks", "10", "--eawr-live-hashes", str(directory / "hidden.hashes.csv")))
            self.assertEqual(code, 0, hidden.get("failure"))
            live = hidden["live_session"]
            self.assertEqual((live["fixture"], live["reveal"]), ("m2", False))
            self.assertGreater(live["hidden_units"], 0)
            self.assertFalse(any(unit["entity"] == EMPIRE_STATION for unit in live["hostile_units"]),
                             live["hostile_units"])

            code, revealed = self._run(directory, "revealed", (
                "--eawr-live-reveal", "on", "--eawr-live-ticks", "10",
                "--eawr-live-hashes", str(directory / "revealed.hashes.csv")))
            self.assertEqual(code, 0, revealed.get("failure"))
            live = revealed["live_session"]
            self.assertEqual((live["fixture"], live["reveal"]), ("m2", True))
            self.assertEqual(live["hidden_units"], 0)
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertTrue(any(unit["entity"] == EMPIRE_STATION for unit in live["hostile_units"]),
                            live["hostile_units"])
            # Presentation-only: reveal never reaches the m2 session, so both runs' ticks hash
            # the same (the AI's decisions, its commands and the recorded state are unchanged).
            self.assertEqual((directory / "hidden.hashes.csv").read_text(encoding="utf-8"),
                             (directory / "revealed.hashes.csv").read_text(encoding="utf-8"))


    def test_replay_resolves_map_object_types(self):
        # #501: an m2 recording's units include its map objects (e.g. unit 12,
        # Skirmish_Merchant_Dock), which are not in the FoC unit tables. Replaying the
        # recording back through --eawr-live-replay must resolve them the same way the live
        # path places them (LiveSessionView::prepare_replay), not just against the unit
        # tables. A plain replay of the same window reaches the exact recorded state
        # (bit-identical, sensors unchanged) with display reveal either off or on.
        ticks = 300
        with tempfile.TemporaryDirectory(prefix="eawr-live-replay-mapobj-") as temporary:
            directory = pathlib.Path(temporary)
            code, recorded = self._run(directory, "record", (
                "--eawr-live-ticks", str(ticks),
                "--eawr-live-replay-out", str(directory / "record.eawr-replay")))
            self.assertEqual(code, 0, recorded.get("failure"))
            live = recorded["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertTrue((directory / "record.eawr-replay").is_file())

            code, replayed = self._run(directory, "replay", (
                "--eawr-live-replay", str(directory / "record.eawr-replay"), "--eawr-live-ticks", str(ticks)),
                session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, replayed.get("failure"))
            replay = replayed["live_session"]
            self.assertIs(replay["reveal"], False)
            self.assertIs(replay["headless_hashes_equal"], True)
            self.assertEqual(replay["final_state_sha256"], live["final_state_sha256"])

            code, revealed = self._run(directory, "replay_revealed", (
                "--eawr-live-replay", str(directory / "record.eawr-replay"), "--eawr-live-ticks", str(ticks),
                "--eawr-live-reveal", "on"), session=("--eawr-live-session", "replay"))
            self.assertEqual(code, 0, revealed.get("failure"))
            reveal = revealed["live_session"]
            self.assertIs(reveal["reveal"], True)
            self.assertIs(reveal["headless_hashes_equal"], True)
            self.assertEqual(reveal["final_state_sha256"], live["final_state_sha256"])
            self.assertEqual(reveal["rejected"], live["rejected"])


    def test_attack_order_turns_the_tartan(self):
        # #361: `--eawr-live-order <tick>:attack:<unit>@<target>` reaches the session as an attack
        # command. In the duel the Tartan (unit 2, player 1) faces the Nebulon-B (unit 1) 800
        # units to its west; a face order turns it north (yaw 90) by about tick 140. Ordered at
        # tick 200 to attack the Nebulon-B, which is within its 800-unit attack distance, it
        # turns in place back onto the frigate's nearest hardpoint (space-weapon-fire A-04);
        # without the order it keeps facing north.
        with tempfile.TemporaryDirectory(prefix="eawr-live-attack-") as temporary:
            directory = pathlib.Path(temporary)
            face = ("--eawr-live-order", "30:face:2@400,0,0")
            args = ("--eawr-live-replay", str(DUEL), "--eawr-live-reveal", "on", "--eawr-live-player", "1",
                    "--eawr-live-ticks", "400")
            runs = {}
            for name, extra in (("faced", face), ("attack", (*face, "--eawr-live-order", "200:attack:2@1"))):
                code, result = self._run(directory, name, (*args, *extra),
                                         session=("--eawr-live-session", "replay"), camera=DUEL_CAMERA)
                self.assertEqual(code, 0, result.get("failure"))
                live = result["live_session"]
                self.assertEqual(live["rejected"], [])
                self.assertIs(live["headless_hashes_equal"], True)
                self.assertGreaterEqual(live["completed_ticks"], 400)
                runs[name] = live
            orders = runs["attack"]["orders"]
            self.assertIn({"tick": 200, "kind": "attack", "unit": 2, "target": 1}, orders)
            yaw = {name: {unit["entity"]: unit["yaw"] for unit in live["own_units"]}[2] for name, live in runs.items()}
            self.assertAlmostEqual(yaw["faced"], 90.0, delta=0.01)
            # The turn back ends on the bearing to the Nebulon-B's nearest hardpoint, within a
            # few degrees of due west.
            self.assertLess(abs(abs(yaw["attack"]) - 180.0), 5.0, yaw)
            self.assertNotEqual(runs["faced"]["final_state_sha256"], runs["attack"]["final_state_sha256"])


    def test_fog_plane_follows_the_local_fog_cells(self):
        # #494 (space-fog-presentation.md FW-01 to FW-13): the Rebel player's fog is drawn in the
        # world from the session's own fog cells, one texel per 100-unit cell of the map's fog grid
        # (Coruscant +-6500: 130 cells, V-18), and the session's hashes are unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fog-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fog", ("--eawr-live-ticks", "40", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            fog = result["live_fog"]
            self.assertEqual(fog["status"], "drawn", fog["notes"])
            self.assertFalse(fog["revealed"])
            self.assertEqual((fog["colour"], fog["height"], fog["cell_size"], fog["regrow_seconds"]),
                             ([255, 255, 255, 255], -80, 100, 6))
            self.assertEqual(fog["fade_step_per_frame"], 21 / 16)
            self.assertTrue(fog["grid_texture"].endswith("w_space_fow_grid.dds"), fog["grid_texture"])
            self.assertEqual(fog["source"], "cells")
            self.assertEqual({key: fog["grid"][key] for key in ("left", "top", "cell", "wide", "tall")},
                             {"left": -6500, "top": 6500, "cell": 100, "wide": 130, "tall": 130})
            self.assertGreater(fog["cell_tick"], 0)
            self.assertGreater(fog["revealers"], 0)
            self.assertGreater(fog["held_cells"], 0)
            # Most of the square is fogged at the start; the Rebel fleet's corner is not.
            self.assertGreater(fog["fogged_cells"], 130 * 130 // 2)
            self.assertGreater(fog["uploads"], 0)
            self.assertGreater(fog["advanced_logical_frames"], 0)


    def test_enemy_ship_ramps_across_the_fog_edge(self):
        # FW-16 to FW-18: the Rebel corvette sails towards the Empire fleet.
        # #1331: on the current M2 roster, TIE craft 51 enters around tick 1597
        # and leaves around tick 2338. Craft 50 only enters before this run ends.
        # Sample the actual two edges without changing the ramp assertions.
        fog_crossing_fighter = 51
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fade-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fade", (
                "--eawr-live-order", f"15:move:{CORVETTE}@2500,-2600,0", "--eawr-live-ticks", "2400",
                "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            rows = [(row["tick"], row["opacity"]) for row in live["fading_log"]
                    if row["entity"] == fog_crossing_fighter]
            rising = [(tick, opacity) for tick, opacity in rows if tick < 2300]
            falling = [(tick, opacity) for tick, opacity in rows if tick >= 2300]
            self.assertGreaterEqual(len(rising), 5, rows)
            self.assertGreaterEqual(len(falling), 5, rows)
            for series, sign in ((rising, 1), (falling, -1)):
                values = [opacity for _, opacity in series]
                self.assertTrue(all(sign * (after - before) >= 0 for before, after in zip(values, values[1:])), series)
                self.assertTrue(all(abs(after - before) <= 0.5 for before, after in zip(values, values[1:])),
                                f"a step, not a ramp: {series}")
                self.assertGreaterEqual(sum(1 for value in values if 0.05 < value < 0.95), 3, series)
            self.assertLess(rising[0][1], 0.5)
            self.assertGreater(rising[-1][1], 0.99)
            self.assertLess(falling[-1][1], 0.1)


    def test_reveal_draws_everything_at_full_opacity(self):
        # #535 with #507: --eawr-live-reveal on shows every unit at full opacity; nothing fades.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fade-reveal-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "revealed", (
                "--eawr-live-reveal", "on", "--eawr-live-order", f"15:move:{CORVETTE}@2500,-2600,0",
                "--eawr-live-ticks", "2400", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["fading_units"], live["fading"], live["fading_log"]), (0, [], []))


    def test_deployment_overlay_draws_the_red_grid(self):
        # #563 (space-fog-presentation.md FW-22, FW-23, K-5): the overlay is opt-in and draws with
        # the reinforcement tile; the session's hashes are unchanged.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-overlay-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "overlay", (
                "--eawr-live-deploy-overlay", "on", "--eawr-live-ticks", "40", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            fog = result["live_fog"]
            self.assertEqual(fog["status"], "drawn", fog["notes"])
            self.assertIs(fog["deploy_overlay"], True)
            self.assertEqual(fog["overlay_colour"], [255, 0, 0, 254])
            self.assertTrue(fog["grid_texture"].endswith("w_space_reinforce_fow_grid.dds"), fog["grid_texture"])


    def test_victory_is_reported(self):
        # #77 (space-victory VT-05, VT-10): the Empire station falls at tick 15; the live session
        # decides the local player's victory and its headless replay, with the same rules, agrees.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-victory-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "victory", (
                "--eawr-live-order", f"15:damage:{EMPIRE_STATION}@1000000", "--eawr-live-ticks", "60",
                "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual(live["rejected"], [])
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["outcome"], {
                "condition": "enemy_starbase_destroyed", "winner": 1, "winner_team": 0, "decided_tick": 15,
                "deciding_unit": EMPIRE_STATION, "end_tick": 225, "local_result": "victory"})


    def test_victory_shows_the_message_and_ends_the_battle(self):
        # #453 (battle-end.md BE-02, BE-03, BEP-01 to BEP-03): "WE ARE VICTORIOUS!" from the frame
        # that carries the outcome, the session halts at end_tick 225 and the end panel opens; the
        # run stops at the end, however many ticks it asked for.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-end-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._battle_end(directory, "won", EMPIRE_STATION, "victory")
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["completed_ticks"], 225, "the session halts at end_tick")
            end = live["battle_end"]
            self.assertEqual((end["message"], end["title"], end["decided_tick"], end["end_tick"], end["halt_tick"]),
                             ("TEXT_WIN_TACTICAL", "TEXT_VICTORY", 15, 225, 225))
            self.assertEqual(end["ended_frame"], 76)
            self.assertEqual(live["time"]["state"], "ended")
            self.assertEqual(live["time"]["track"][-1]["cause"], "end")
            overlay = result["hud"]["battle_overlay"]
            self.assertEqual(overlay["message"]["text"], "WE ARE VICTORIOUS!")
            self.assertEqual(overlay["end_panel"]["title"], "Victory!")
            # BE-03: centred, its top at 0.4 of the height.
            x, y, width, _ = overlay["message"]["rect"]
            self.assertAlmostEqual(x + width / 2, 640.0, delta=1.0)
            self.assertAlmostEqual(y, 288.0, delta=1.0)
            # The message is on the frame after the outcome and not before; the panel at the end.
            images = [decode_png((directory / name).read_bytes()) for name in ("won_t0012.png", "won_t0018.png", "won_f0080.png")]
            self.assertNotEqual(images[0][2], images[1][2])
            self.assertNotEqual(images[1][2], images[2][2])
            self.assertEqual((directory / "won.eawr-replay.time.csv").read_text(encoding="utf-8").splitlines(),
                             ["tick,state,ticks_per_second", "225,ended,0"])


    def test_defeat_shows_the_message_and_quit_ends_the_run(self):
        # #453: the Rebel station falls: "WE HAVE BEEN DEFEATED!", then "Defeat!"; the end panel's
        # Quit Game ends the run before its remaining frames.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-defeat-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._battle_end(directory, "lost", STATION, "defeat",
                                            ("--eawr-live-input", "f80:click:hud=quit"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertEqual((live["battle_end"]["message"], live["battle_end"]["title"]),
                             ("TEXT_LOSE_TACTICAL", "TEXT_DEFEAT"))
            self.assertEqual(live["outcome"]["local_result"], "defeat")
            self.assertIs(live["battle_end"]["quit"], True)
            self.assertEqual(live["completed_ticks"], 225)
            overlay = result["hud"]["battle_overlay"]
            self.assertEqual(overlay["message"]["text"], "WE HAVE BEEN DEFEATED!")
            self.assertEqual((overlay["end_panel"]["title"], overlay["quit_presses"]), ("Defeat!", 1))
            # Quit ends the run on frame 82 (the pointer reaches the button on 80, the click is
            # sent on 81 and dispatched on 82), before the 30 warm-up and 120 timed frames end.
            self.assertLess(live["frames"], 120)


    def test_time_panel_pauses_and_fast_forwards_without_changing_the_hashes(self):
        # #459 (tactical-time-controls.md TM-05 to TM-09, TP-01 to TP-06): the time panel's buttons,
        # clicked as a player would (the pointer reaches a button on the frame given, the click is
        # dispatched two frames later). Pause at frame 10,
        # a fast-forward press while paused does nothing, Resume Game plays, fast forward runs four
        # times as many ticks per frame, then back to normal speed and another pause and play. The
        # debug orders run at their ticks either way, so a plain run ends on the same state.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-time-") as temporary:
            directory = pathlib.Path(temporary)
            inputs = []
            for gesture in ("f10:click:hud=pause", "f12:click:hud=fast_forward", "f20:click:hud=resume",
                            "f22:click:hud=fast_forward", "f30:click:hud=fast_forward", "f32:click:hud=pause",
                            "f34:click:hud=pause"):
                inputs += ["--eawr-live-input", gesture]
            code, result = self._run(directory, "time", (
                *ORDERS, *inputs, "--eawr-live-ticks", "240", "--eawr-live-step", "3", "--eawr-live-workers", "2",
                "--eawr-live-capture-frames", "10,15,25", "--eawr-live-hashes", str(directory / "time.hashes.csv"),
                "--eawr-live-replay-out", str(directory / "time.eawr-replay")))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            track = [(row["state"], row["ticks_per_second"], row["cause"]) for row in live["time"]["track"]]
            self.assertEqual(track, [("paused", 0, "pause"), ("play", 30, "resume"),
                                     ("fast_forward", 120, "fast_forward"), ("play", 30, "normal_speed"),
                                     ("paused", 0, "pause"), ("play", 30, "play")])
            ticks = [row["tick"] for row in live["time"]["track"]]
            self.assertEqual(ticks[0], ticks[1], "no tick ran while paused")
            self.assertEqual(ticks[2] - ticks[1], 6, "two frames at step 3 from Resume Game to fast forward")
            self.assertEqual(ticks[3] - ticks[2], 8 * 12, "eight fast-forward frames of 12 ticks")
            panel = result["hud"]["time_panel"]
            self.assertEqual(panel["fast_forward"]["presses"], 2, "the press while paused never reached the button")
            self.assertEqual(panel["pause"]["presses"], 3)
            self.assertEqual(result["hud"]["battle_overlay"]["resume_presses"], 1)
            csv = (directory / "time.eawr-replay.time.csv").read_text(encoding="utf-8").splitlines()
            self.assertEqual(csv[0], "tick,state,ticks_per_second")
            self.assertEqual(len(csv), 7)
            # Paused frames show the banner and hold the battle: frames 15 and 25 straddle Resume Game.
            paused = decode_png((directory / "time_f0015.png").read_bytes())
            playing = decode_png((directory / "time_f0025.png").read_bytes())
            self.assertNotEqual(paused[2], playing[2])
            code, plain = self._run(directory, "plain", (
                *ORDERS, "--eawr-live-ticks", "240", "--eawr-live-step", "3", "--eawr-live-workers", "2",
                "--eawr-live-hashes", str(directory / "plain.hashes.csv")))
            self.assertEqual(code, 0, plain.get("failure"))
            # Both runs draw at least the 150 warm-up and timed frames, so fast forward reaches
            # further; every tick both ran has the same hash.
            timed = (directory / "time.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            untimed = (directory / "plain.hashes.csv").read_text(encoding="utf-8").splitlines()[1:]
            common = min(len(timed), len(untimed))
            self.assertGreaterEqual(common, 240)
            self.assertEqual(timed[:common], untimed[:common])


    def test_an_order_given_while_paused_runs_at_the_next_tick(self):
        # #459 TM-10, TP-03: select the corvette and order it on while paused; the order is stamped
        # for the tick the pause stopped before, and runs there once the battle plays.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-paused-order-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "queued", (
                "--eawr-live-input", "f10:click:hud=pause", "--eawr-live-input", f"f12:click:unit={CORVETTE}",
                "--eawr-live-input", "f13:rclick:@-4000,5500,0", "--eawr-live-input", "f16:click:hud=pause",
                "--eawr-live-ticks", "90", "--eawr-live-step", "3", "--eawr-live-workers", "2"))
            self.assertEqual(code, 0, result.get("failure"))
            live = result["live_session"]
            self.assertIs(live["headless_hashes_equal"], True)
            self.assertEqual(live["rejected"], [])
            paused_at = live["time"]["track"][0]["tick"]
            notes = [note for note in result["battle_input"]["log"] if note.startswith("move @")]
            self.assertEqual(len(notes), 1, result["battle_input"]["log"])
            self.assertTrue(notes[0].endswith(f" tick {paused_at}"), (notes, paused_at))


    def test_simulation_fault_is_reported(self):
        # #316 review 2: an exception on the simulation thread fails the capture run in order,
        # with the error in the report, instead of terminating the process.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-fault-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "fault", (
                "--eawr-live-fault-tick", "6", "--eawr-live-capture-ticks", "0,30"))
            # The hook's std::runtime_error comes from the extension, built with
            # _HAS_EXCEPTIONS=0, so the session sees it as a non-standard exception.
            self.assertEqual(code, 2, result.get("failure"))
            self.assertIn("simulation thread stopped before tick 6", result["failure"])
            live = result["live_session"]
            self.assertEqual(live["status"], "simulation_failed")
            self.assertIn("simulation thread stopped before tick 6", live["error"])
            self.assertEqual(live["completed_ticks"], 6)
            # #615: the replay through the failed tick lands beside godot.log (user://logs).
            replay = pathlib.Path(live["failure_replay"])
            try:
                self.assertEqual(replay.parent.name, "logs", replay)
                self.assertRegex(replay.name, r"^eawr-live-failure-[0-9]+-pid[0-9]+-usec[0-9]+-tick7\.eawr-replay$")
                self.assertTrue(replay.read_bytes().startswith(b"EAWRPLY"), replay)
            finally:
                replay.unlink(missing_ok=True)


    def test_capture_ticks_are_the_ticks_shown(self):
        # #316 review 3: with coarse steps a capture tick must be one a frame shows, and the file
        # is named after it.
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-step-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "between", (
                "--eawr-live-step", "30", "--eawr-live-capture-ticks", "5"))
            self.assertNotEqual(code, 0)
            self.assertIn("falls between frames", result["failure"])
            code, result = self._run(directory, "coarse", (
                "--eawr-live-step", "30", "--eawr-live-capture-ticks", "0,30,60"))
            self.assertEqual(code, 0, result.get("failure"))
            for name in ("coarse_t0000.png", "coarse_t0030.png", "coarse_t0060.png"):
                self.assertIn(name, result["captures"])
            # This run plays the battle for thousands of ticks. An FoC AI order may meet a target
            # that died in the tick before (target_not_live), which is the battle's course, not
            # the capture path (#520: PC-08 changes that course); no other order is rejected.
            live = result["live_session"]
            ai_players = set(live["ai"]["players"])
            self.assertEqual([entry for entry in live["rejected"]
                              if not (any(f"player {player}," in entry for player in ai_players)
                                      and "target_not_live" in entry)], [])


    def test_quit_after_teardown_stress(self):
        executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
        self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
        with tempfile.TemporaryDirectory(prefix="eawr-live-teardown-") as temporary:
            directory = pathlib.Path(temporary)
            for frames in (300, 450, 600, 900):
                for mode, map_name in (("m2", CORUSCANT),
                                       ("skirmish", "data/art/maps/_mp_space_polus.ted")):
                    with self.subTest(frames=frames, mode=mode):
                        completed = subprocess.run(
                            [executable, "--resolution", "1280x720", "--path",
                             str(ROOT / "apps/viewer/project"), "--quit-after", str(frames), "--",
                             "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                             "--eawr-map", map_name, "--eawr-populate", "--eawr-camera-interactive",
                             "--eawr-live-session", mode, "--eawr-live-ai", "on",
                             "--eawr-live-begin", "auto",
                             "--eawr-map-effects", "on", "--eawr-audio", "off",
                             "--eawr-lighting", "sh", "--eawr-environment", "map",
                             "--eawr-shadows", "on", "--eawr-hud", "tactical",
                             "--eawr-report", str(directory / f"{mode}-{frames}.json")],
                            cwd=ROOT, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=180, check=False)
                        self.assertEqual(completed.returncode, 0, completed.stdout[-4000:])
                        self.assertIn("EAWR live session started", completed.stdout)
                        self.assertNotIn("headless replay done", completed.stdout)
                        self.assert_map_teardown_before_host_destruction(completed.stdout)


    @unittest.skipUnless(sys.platform == "win32", "closes the viewer's window through Win32")
    def test_window_close_quits_promptly_and_writes_the_report(self):
        # The owner's close of a running battle took seconds: the teardown replayed the whole battle
        # headlessly to compare hashes (one thread, about as long as the battle), and the report was
        # never written. An interactive session skips that replay unless --eawr-live-verify on asks
        # for it, and the window's close request writes the report before the engine quits.
        from window_close import close_windows_of

        with tempfile.TemporaryDirectory(prefix="eawr-live-quit-") as temporary:
            directory = pathlib.Path(temporary)
            report = directory / "quit.json"
            log = directory / "quit.log"
            executable = os.environ.get("EAWR_GODOT_EXECUTABLE")
            self.assertTrue(executable, "EAWR_GODOT_EXECUTABLE must name the pinned Godot binary")
            with log.open("wb") as output:
                process = subprocess.Popen(
                    [executable, "--resolution", "1280x720", "--path", str(ROOT / "apps/viewer/project"), "--",
                     "--eawr-map", CORUSCANT, "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"],
                     "--eawr-report", str(report), "--eawr-populate", "--eawr-camera-interactive",
                     "--eawr-map-camera-config", str(CAMERA), "--eawr-live-session", "m2",
                     "--eawr-live-begin", "auto",
                     "--eawr-live-ai", "on", "--eawr-hud", "tactical"],
                    cwd=ROOT, stdout=output, stderr=subprocess.STDOUT)
            try:
                deadline = time.monotonic() + 300
                while b"EAWR live session started" not in log.read_bytes():
                    self.assertIsNone(process.poll(), log.read_text(errors="replace")[-4000:])
                    self.assertLess(time.monotonic(), deadline, "the live session never started")
                    time.sleep(0.5)
                # The battle runs; the AI plans, the fleets move.
                time.sleep(LIVE_QUIT_BATTLE_SECONDS)
                self.assertIsNone(process.poll(), log.read_text(errors="replace")[-4000:])
                self.assertGreater(close_windows_of(process.pid), 0, "no viewer window to close")
                closed_at = time.perf_counter()
                # The rig's ordinary wait also publishes caches and retains
                # private evidence. Measure process exit before that work.
                wait_for_exit = getattr(process, "_eawr_wait_for_exit", process.wait)
                wait_for_exit(timeout=60)
                exited_at = time.perf_counter()
                exit_seconds = exited_at - closed_at
            finally:
                if process.poll() is None:
                    process.kill()
                process.wait()
            post_exit_seconds = time.perf_counter() - exited_at
            text = log.read_text(errors="replace")
            # Keep the shutdown stages even when the real exit budget fails (tracked in #1178).
            stages = [{"milliseconds": int(ms), "stage": stage}
                      for ms, stage in re.findall(r"EAWR shutdown \+(\d+) ms ([^\r\n]+)", text)]
            profile = {"close_to_process_exit_seconds": exit_seconds,
                       "runner_post_exit_seconds": post_exit_seconds,
                       "stages": stages}
            (directory / "shutdown-profile.json").write_text(json.dumps(profile, indent=2), encoding="utf-8")
            self.assertEqual(process.returncode, 0, text[-4000:])
            self.assert_map_teardown_before_host_destruction(text)
            self.assertNotIn("headless replay done", text)
            self.assertIn("EAWR shutdown", text)
            self.assertTrue(report.is_file(), text[-4000:])
            result = strict_json(report.read_text(encoding="utf-8"))
            self.assertEqual(result["status"], "space_environment_closed", result.get("failure"))
            live = result["live_session"]
            self.assertGreater(live["completed_ticks"], 0)
            self.assertIsNone(live["headless_hashes_equal"])
            self.assertLess(exit_seconds, LIVE_QUIT_EXIT_BOUND_SECONDS, text[-4000:])


    def test_bad_orders_are_refused(self):
        with tempfile.TemporaryDirectory(prefix="eawr-live-session-bad-") as temporary:
            directory = pathlib.Path(temporary)
            code, result = self._run(directory, "unknown", ("--eawr-live-order", "5:move:999@0,0,0"))
            self.assertNotEqual(code, 0)
            self.assertIn("is not a unit of the start", result["failure"])
            code, result = self._run(directory, "malformed", ("--eawr-live-order", "5:jump:5"))
            self.assertNotEqual(code, 0)
            self.assertIn("--eawr-live-order expects", result["failure"])
            code, result = self._run(directory, "damage", ("--eawr-live-order", "5:damage:5@1,2,3"))
            self.assertNotEqual(code, 0)
            self.assertIn("<tick>:damage:<unit>@<amount>[,<hardpoint>]", result["failure"])
            code, result = self._run(directory, "hardpoint", ("--eawr-live-order", "5:damage:5@1,0.5"))
            self.assertNotEqual(code, 0)
            self.assertIn("<tick>:damage:<unit>@<amount>[,<hardpoint>]", result["failure"])
            for order in ("5:attack:5", "5:attack:5@0", "5:attack:5@x"):
                code, result = self._run(directory, "attack", ("--eawr-live-order", order))
                self.assertNotEqual(code, 0, order)
                self.assertIn("<tick>:attack:<unit>@<target unit>", result["failure"])
            # #449: a follow group names a tick and unit IDs, in tick order.
            for follow in (("5",), ("5:",), ("5:0",), ("x:5",), ("30:5", "10:6")):
                arguments = tuple(item for value in follow for item in ("--eawr-live-follow-group", value))
                code, result = self._run(directory, "follow", arguments)
                self.assertNotEqual(code, 0, follow)
                self.assertIn("--eawr-live-follow-group expects", result["failure"])
