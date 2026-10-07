"""Hardpoint tooltip GPU cases, collected through test_battle_input."""

from battle_input_test_support import CAMERA, HUD_OFF, VIEWPORT, pathlib, re, shutil, tempfile


class HardpointTooltipCases:
    def test_allied_community_station_hardpoint_repair_click(self):
        from test_pad_capture import replay_units
        import zlib

        with tempfile.TemporaryDirectory(prefix="eawr-allied-repair-") as temporary:
            directory = pathlib.Path(temporary)
            replay = directory / "team-start.eawr-replay"
            setup = ["--eawr-skirmish-players", "1,2,3,4"]
            for slot in ("1:Rebel:0:human", "2:Empire:1:human", "3:Rebel:0:human", "4:Empire:1:human"):
                setup += ["--eawr-skirmish-slot", slot]
            for player in (1, 2, 3, 4):
                setup += ["--eawr-skirmish-fleet", f"{player}:none"]
            common = (*HUD_OFF, *setup, "--eawr-live-ai", "off", "--eawr-audio", "off",
                      "--eawr-live-player", "3", "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                      "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on")
            code, initial = self._run(directory, "team-start", (*common,
                "--eawr-live-replay-out", str(replay)), session="skirmish", end_tick=1)
            self.assertEqual(code, 0, initial.get("failure"))
            station = next(unit for unit in replay_units(replay)
                           if unit[1] == zlib.crc32(b"SKIRMISH_REBEL_STAR_BASE_1"))
            self.assertEqual(station[2], 1, "The repairing player 3 must be a nonowner teammate")
            entity = station[0]
            damaged = (*common, "--eawr-live-follow", str(entity),
                       "--eawr-live-order", f"1:damage:{entity}@631,3",
                       "--eawr-live-input", f"20:click:unit={entity}",
                       "--eawr-live-input", f"25:hover:reticle={entity}:3")
            code, baseline = self._run(directory, "ally-no-repair", damaged, session="skirmish", end_tick=80)
            self.assertEqual(code, 0, baseline.get("failure"))
            code, repaired = self._run(directory, "ally-repair", (*damaged,
                "--eawr-live-input", f"30:click:reticle={entity}:3"), session="skirmish", end_tick=80)
            self.assertEqual(code, 0, repaired.get("failure"))
            self.assertEqual(repaired["battle_input"]["selected"], [entity])
            self.assertTrue(any(f"repair {entity} hardpoint 3" in row
                                for row in repaired["battle_input"]["log"]), repaired["battle_input"])
            self.assertTrue(any(row["id"] == "POINTER_REPAIR_HARDPOINT"
                                for row in repaired["battle_input"]["cursor_samples"]))
            self.assertGreater(repaired["world_ui"]["hardpoint_tooltip"]["health"],
                               baseline["world_ui"]["hardpoint_tooltip"]["health"])
            def credits(report):
                return int(report["live_session"]["economy"]["credits"].replace(",", ""))
            self.assertLess(credits(repaired), credits(baseline), "The repairing teammate pays")
            self.assertEqual(repaired["live_session"]["rejected"], [])
            self.assertTrue(repaired["live_session"]["headless_hashes_equal"])
            self.assertTrue(baseline["live_session"]["headless_hashes_equal"])

    def _assert_tooltip_origin(self, result):
        tooltip = result["world_ui"]["hardpoint_tooltip"]
        self.assertIsNotNone(tooltip, result["battle_input"])
        pointer = next(row["at"] for row in result["battle_input"]["scripted_points"] if row["tick"] == 25)
        x, y, width, height = tooltip["rect"]
        # WU-53 clamps unpadded content, then adds the frame's two margins.
        content = (width - 2 * .0035 * VIEWPORT[0], height - 2 * .0035 * VIEWPORT[1])
        limits = (.98 * VIEWPORT[0] - content[0], .98 * VIEWPORT[1] - content[1])
        self.assertAlmostEqual(x, min(pointer[0], limits[0]), delta=.02)
        self.assertAlmostEqual(y, min(pointer[1] + 32, limits[1]), delta=.02)
        return pointer, limits

    def test_station_hardpoint_repair_click(self):
        with tempfile.TemporaryDirectory(prefix="eawr-station-repair-") as temporary:
            directory = pathlib.Path(temporary)
            extra = (
                "--eawr-live-order", "1:damage:1@631,3",
                "--eawr-live-input", "20:hover:unit=1",
                "--eawr-live-input", "25:hover:reticle=1:3",
                "--eawr-live-input", "30:click:reticle=1:3")
            code, result = self._tooltip_run(directory, "repair-click", extra)
            self.assertEqual(code, 0, result.get("failure"))
            self.assertTrue(any("repair 1 hardpoint 3" in row for row in result["battle_input"]["log"]), result["battle_input"])
            tooltip = result["world_ui"]["hardpoint_tooltip"]
            self.assertIsNotNone(tooltip, result["world_ui"])
            self.assertEqual((tooltip["entity"], tooltip["hardpoint"]), (1, 3))
            self.assertGreater(tooltip["health"], 344 / 675)
            self.assertLess(tooltip["health"], 1)
            self.assertTrue(any(row["id"] == "POINTER_REPAIR_HARDPOINT" for row in result["battle_input"]["cursor_samples"]),
                            result["battle_input"]["cursor_samples"])
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)

            # An armed attack click on our hardpoint cancels/selects; it does not pay for repair.
            code, armed = self._tooltip_run(directory, "repair-armed-click", (
                *extra[:6], "--eawr-live-input", "28:key:A", *extra[6:]))
            self.assertEqual(code, 0, armed.get("failure"))
            self.assertIn("attack mode", armed["battle_input"]["log"])
            self.assertFalse(any("repair 1 hardpoint 3" in row for row in armed["battle_input"]["log"]), armed["battle_input"])
            self.assertAlmostEqual(armed["world_ui"]["hardpoint_tooltip"]["health"], 344 / 675, places=5)
            self.assertIs(armed["live_session"]["headless_hashes_equal"], True)

    def test_hardpoint_tooltip_viewport_edges(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hardpoint-edges-") as temporary:
            directory = pathlib.Path(temporary)
            shutil.copy(CAMERA.parent / "space-live-camera-bindings.json", directory)
            for name, target, axis in (("right", (-4661, 4460), 0), ("bottom", (-3811, 5050), 1)):
                with self.subTest(edge=name):
                    camera = directory / f"{name}-camera.xml"
                    camera.write_text(re.sub(r"<initial [^>]*/>",
                        f'<initial target_x="{target[0]}" target_y="{target[1]}" '
                        'target_height="0" zoom="0.611111" yaw_degrees="0"/>',
                        CAMERA.read_text(encoding="utf-8")), encoding="utf-8")
                    # Keep the authored camera fixed rather than following the station.
                    code, result = self._run(directory, name, (
                        *HUD_OFF, "--eawr-live-ai", "off", "--eawr-live-player", "1",
                        "--eawr-live-reveal", "on", "--eawr-live-step", "1",
                        "--eawr-environment", "map", "--eawr-lighting", "sh", "--eawr-shadows", "on",
                        "--eawr-live-order", "1:damage:1@631,3",
                        "--eawr-live-input", "20:hover:unit=1",
                        "--eawr-live-input", "25:hover:reticle=1:3"), camera=camera, end_tick=80)
                    self.assertEqual(code, 0, result.get("failure"))
                    tooltip = result["world_ui"]["hardpoint_tooltip"]
                    self.assertIsNotNone(tooltip, result["battle_input"])
                    self.assertEqual((tooltip["entity"], tooltip["hardpoint"]), (1, 3))
                    self.assertEqual(tooltip["text"], "Laser Cannon Battery - 51%")
                    pointer, limits = self._assert_tooltip_origin(result)
                    self.assertTrue(all(0 <= pointer[i] < VIEWPORT[i] for i in range(2)), pointer)
                    self.assertGreater(pointer[axis] + (32 if axis else 0), limits[axis],
                                       "Fixture must exercise the clamped edge")
                    self.assertIs(result["live_session"]["headless_hashes_equal"], True)

    def _tooltip_run(self, directory, name, extra, end_tick=80, hud=False):
        return self._run(directory, name, (
            *(() if hud else HUD_OFF), "--eawr-live-ai", "off", "--eawr-live-player", "1",
            "--eawr-live-follow", "1", "--eawr-live-reveal", "on",
            "--eawr-live-step", "1", "--eawr-environment", "map",
            "--eawr-lighting", "sh", "--eawr-shadows", "on", *extra), end_tick=end_tick)

    def test_hardpoint_tooltip_damaged_and_destroyed(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hardpoint-tooltip-") as temporary:
            directory = pathlib.Path(temporary)
            # Drain 300 shields, then damage 331 of the laser's effective
            # 675 health (450 XML x 1.5 space multiplier): 344/675 rounds to 51%.
            code, result = self._tooltip_run(directory, "damaged", (
                "--eawr-live-order", "1:damage:1@631,3",
                "--eawr-live-input", "20:hover:unit=1",
                "--eawr-live-input", "25:hover:reticle=1:3"))
            self.assertEqual(code, 0, result.get("failure"))
            tooltip = result["world_ui"]["hardpoint_tooltip"]
            self.assertIsNotNone(tooltip, result["battle_input"])
            self.assertEqual((tooltip["entity"], tooltip["hardpoint"]), (1, 3))
            self.assertEqual(tooltip["text"], "Laser Cannon Battery - 51%")
            self.assertAlmostEqual(tooltip["health"], 344 / 675, places=5)
            x, y, width, height = tooltip["rect"]
            self.assertGreater(width, 128)
            self.assertGreater(height, 0)
            self._assert_tooltip_origin(result)
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
            code, result = self._tooltip_run(directory, "destroyed", (
                "--eawr-live-order", "1:damage:1@100000,3",
                "--eawr-live-input", "20:hover:unit=1",
                "--eawr-live-input", "25:hover:reticle=1:3"))
            self.assertEqual(code, 0, result.get("failure"))
            self.assertIsNone(result["world_ui"]["hardpoint_tooltip"])
            self.assertTrue(any("target is not on screen" in row for row in result["battle_input"]["log"]), result["battle_input"])

    def test_hardpoint_tooltip_zero_health_station_carryover(self):
        with tempfile.TemporaryDirectory(prefix="eawr-hardpoint-carryover-") as temporary:
            directory = pathlib.Path(temporary)
            # Determine the ordinary allocator's replacement identity from the real upgrade.
            extra = (
                "--eawr-live-order", "31:damage:1@100000,3",
                "--eawr-live-input", "1:click:unit=1",
                "--eawr-live-input", "10:click:card=2",
                "--eawr-live-input", "20:hover:unit=1",
                "--eawr-live-input", "25:hover:reticle=1:3",
                "--eawr-live-input", "30:click:card=4")
            code, probe = self._tooltip_run(directory, "upgrade-probe", extra, end_tick=2500, hud=True)
            self.assertEqual(code, 0, probe.get("failure"))
            self.assertEqual(probe["live_session"]["economy"]["tech_level"], 2)
            station = probe["battle_input"]["selected"][0]
            self.assertNotEqual(station, 1)
            # The upgrade retains these inherited attachment positions. Use the
            # real projected points: initial-unit CLI selectors reject replacements.
            points = {row["tick"]: row["at"] for row in probe["battle_input"]["scripted_points"]}
            unit_point = f"screen={points[20][0]},{points[20][1]}"
            reticle_point = f"screen={points[25][0]},{points[25][1]}"
            code, result = self._tooltip_run(directory, "carryover", (
                *extra, "--eawr-live-input", f"2450:hover:{unit_point}",
                "--eawr-live-input", f"2460:hover:{reticle_point}"), end_tick=2500, hud=True)
            self.assertEqual(code, 0, result.get("failure"))
            tooltip = result["world_ui"]["hardpoint_tooltip"]
            self.assertIsNotNone(tooltip, result["battle_input"])
            self.assertEqual(tooltip["text"], "Laser Cannon Battery - 0%")
            self.assertEqual((tooltip["entity"], tooltip["hardpoint"]), (station, 3))
            self.assertLess(tooltip["health"], .005)
            self.assertIs(result["live_session"]["headless_hashes_equal"], True)
