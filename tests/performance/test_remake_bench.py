"""Keep benchmark summaries honest when a rig output is missing or incomplete."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("remake_bench", ROOT / "tools/performance/remake_bench.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class RemakeBenchTests(unittest.TestCase):
    def test_rejections_match_the_recipes_actual_tick_and_owner(self):
        native = self.native_start()["live_session"]
        other = "EAWR-SIM-0309 command (tick 5, player 1, sequence 3): unit_not_live"
        fixture = "EAWR-SIM-0309 command (tick 5, player 2, sequence 4): unit_not_live"
        native["rejected"] = [other, fixture]
        result = bench.order_rejections(native, dict(orders=["5:attack_move:305@20,10,0"]))
        self.assertEqual(result["fixture_order_rejections"], [fixture])
        self.assertEqual(result["ai_order_rejections"], [other])
        self.assertEqual(result["unattributed_order_rejections"], [])

    def test_later_ai_rejections_do_not_invalidate_accepted_fixture_orders(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            native = self.native_start()
            recipe = bench.engagement_recipe(native)
            later = "EAWR-SIM-0309 command (tick 961, player 2, sequence 1020): 1 of 1 unit orders rejected (first: ability_unavailable)"
            fixture = "EAWR-SIM-0309 command (tick 1, player 1, sequence 1): 1 of 2 unit orders rejected (first: unit_not_live)"
            native["live_session"]["rejected"] = [later]
            (root / "exit-codes.txt").write_text("stock-bespin 0\n")
            (root / "stock-bespin.csv").write_text("tick,wall_frame_ms,projectiles\n600,20,1\n2300,30,1\n")
            (root / "_spec.json").write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=dict(engagement=recipe))])))
            path = root / "stock-bespin.json"
            path.write_text(json.dumps(native))
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertNotIn("measurement_error", row)
            self.assertEqual(row["ai_order_rejections"], [later])
            self.assertEqual(row["fixture_order_rejections"], [])
            native["live_session"]["rejected"].append(fixture)
            path.write_text(json.dumps(native))
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertIn("scripted fixture", row["measurement_error"])
            self.assertEqual(row["fixture_order_rejections"], [fixture])
            self.assertEqual(row["ai_order_rejections"], [later])
            native["live_session"]["rejected"] = ["unrecognized rejection diagnostic"]
            path.write_text(json.dumps(native))
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertIn("unattributed", row["measurement_error"])

    def test_bounded_logger_window_does_not_shift_with_first_contact(self):
        name = "raw-late-fleet60-file-control"
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "exit-codes.txt").write_text(name + " 0\n")
            (root / (name + ".csv")).write_text(
                "frame,tick,wall_frame_ms,projectiles\n1000,500,10,0\n1100,550,20,1\n1400,700,30,2\n")
            (root / "_spec.json").write_text(json.dumps(dict(runs=[dict(name=name,
                benchmark=dict(window=[1200, 3500], log_mode="file"))])))
            (root / (name + ".log")).write_text("")
            row = bench.analyse_scene(root, name, bench.SCENES[name])
            self.assertEqual(row["window_ticks"], [500, 700])
            self.assertEqual(row["frames_window"], 3)
            self.assertTrue(row["combat_projectiles_observed"])
            self.assertTrue(row["analysis_window_post_hoc"])
            self.assertEqual(row["recorded_protocol_window"], [1200, 3500])

    def test_log_growth_brackets_the_selected_frame_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "x.observer.csv").write_text("frame,wall_frame_ms\n0,0\n1,1000\n2,1000\n3,1000\n")
            (root / "x.observer-startup.json").write_text("{}")
            (root / "x.proc.info").write_text("logical_processors=4")
            os.utime(root / "x.observer-startup.json", (1000, 1000))
            os.utime(root / "x.proc.info", (999.9, 999.9))
            (root / "x.proc.csv").write_text("elapsed_ms,log_bytes,log_error_markers,shader_limit_errors,log_tracking_valid\n0,10,1,0,1\n1000,20,3,1,1\n2000,30,5,2,1\n3000,40,8,4,1\n")
            result = bench.window_log_stats(root, "x", [{"frame": "1"}, {"frame": "2"}])["log_growth_window"]
            self.assertEqual(result["elapsed_ms"], [0, 3000])
            self.assertEqual(result["bytes"], 30)
            self.assertEqual(result["bytes_per_s"], 10)
            self.assertEqual(result["log_error_markers"], 7)
            self.assertEqual(result["shader_limit_errors"], 4)
            self.assertEqual(result["observer_boundary_indices"], [0, 2])
            self.assertIn("approximate", result["alignment"])
            (root / "x.proc.csv").write_text("elapsed_ms,log_bytes\n0,10\n1000,20\n2000,30\n3000,40\n")
            legacy = bench.window_log_stats(root, "x", [{"frame": "1"}, {"frame": "2"}])["log_growth_window"]
            self.assertIsNone(legacy["log_error_markers"])

    def test_native_log_window_uses_preceding_post_draw_intervals(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "x.observer.csv").write_text("frame,wall_frame_ms\n0,0\n1,500\n2,700\n3,900\n4,1100\n")
            (root / "x.observer-startup.json").write_text("{}")
            (root / "x.proc.info").write_text("logical_processors=4")
            os.utime(root / "x.observer-startup.json", (1000, 1000))
            os.utime(root / "x.proc.info", (999.9, 999.9))
            (root / "x.proc.csv").write_text("elapsed_ms,log_bytes\n" + "".join(f"{i*500},{(i+1)*10}\n" for i in range(8)))
            window = [dict(frame="3", wall_frame_ms="700"), dict(frame="4", wall_frame_ms="900")]
            result = bench.window_log_stats(root, "x", window, native_begin_frame=True)["log_growth_window"]
            self.assertEqual(result["observer_boundary_indices"], [1, 3])
            self.assertEqual(result["elapsed_ms"], [500, 2500])
            self.assertEqual(result["bytes"], 40)
            self.assertEqual(result["clock_check"]["absolute_difference_ms"]["median"], 0)
            window[1]["wall_frame_ms"] = "1000"
            invalid = bench.window_log_stats(root, "x", window, native_begin_frame=True)
            self.assertNotIn("log_growth_window", invalid)
            self.assertIn("alignment", invalid["log_window_error"])

    def test_logger_controls_share_scene_args_and_use_prepared_source(self):
        self.assertEqual(len(bench.select("logger-control")), 3)
        for source in ("remake-menu", "raw-late-fleet60", "remake-dev4"):
            for mode in ("file", "pipe", "nul"):
                scene = bench.SCENES[f"{source}-{mode}-control"]
                if source != "raw-late-fleet60":
                    self.assertEqual(scene["args"], bench.SCENES[source]["args"])
                self.assertEqual(scene["log_mode"], mode)
        controls = [bench.SCENES[f"raw-late-fleet60-{mode}-control"] for mode in ("file", "pipe", "nul")]
        self.assertTrue(all(s["args"] == controls[0]["args"] for s in controls))
        self.assertTrue(all(s["window"] == (500, 700) and s["ticks"] == 750 for s in controls))
        args = controls[0]["args"]
        self.assertEqual(args[args.index("--eawr-live-ticks") + 1], "750")
        self.assertEqual(args[args.index("--eawr-live-capture-ticks") + 1], "0,750")
        self.assertEqual(bench.SCENES["raw-late-fleet60"]["window"], (1200, 3500))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / "remake-dev4.json").write_text(json.dumps(self.native_start()))
            loaded = bench.load_preparations(path, ["remake-dev4-nul-control"])
            self.assertEqual(loaded["remake-dev4-nul-control"], self.native_start())

    @staticmethod
    def native_start():
        return dict(live_session=dict(start_seed=4645, start_map="data/art/maps/fixture.ted", start_map_sha256="a" * 64,
                    start_fleet=[dict(entity=91, player=1, type="A", position=[-100, 40, 0]),
                                 dict(entity=7, player=1, type="A", position=[-90, 30, 0]),
                                 dict(entity=305, player=2, type="B", position=[150, -30, 0])],
                    start_markers=[dict(player=1, use="spawn", position=[-100, 40, 0]),
                                   dict(player=2, use="spawn", position=[140, -20, 0])]))

    def test_menu_uses_frame_window_and_actual_distribution(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "exit-codes.txt").write_text("remake-menu 0\n")
            (root / "remake-menu.observer.csv").write_text(
                "frame,tick,wall_frame_ms,draw_calls\n599,1000,900,1\n600,0,10,2\n601,0,20,3\n3500,0,30,4\n3501,0,800,1\n")
            row = bench.analyse_scene(root, "remake-menu", bench.SCENES["remake-menu"])
            self.assertNotIn("measurement_error", row)
            self.assertEqual(row["result_median_frame_ms"], 20)
            self.assertEqual(row["result_p95_frame_ms"], 30)
            self.assertEqual(row["frames_window"], 3)
            self.assertIsNone(row["main_ms"]["median"])
            json.dumps(row, allow_nan=False)

    def test_aggregate_mean_is_never_a_median(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "remake-menu.json").write_text('{"frame_ms_mean": 42}')
            row = bench.analyse_scene(root, "remake-menu", bench.SCENES["remake-menu"])
            self.assertNotIn("result_median_frame_ms", row)
            self.assertIn("measurement_error", row)

    def test_short_or_failed_native_run_is_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "stock-bespin.csv").write_text("tick,wall_frame_ms\n600,10\n1000,20\n")
            (root / "exit-codes.txt").write_text("stock-bespin 0\n")
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertIn("measurement_error", row)
            (root / "stock-bespin.csv").write_text("tick,wall_frame_ms\n600,10\n2300,20\n")
            (root / "exit-codes.txt").write_text("stock-bespin 1\n")
            self.assertIn("measurement_error", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"]))

    def test_floor_stop_is_reported_as_a_memory_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "remake-dev4.floor.json").write_text(json.dumps({"result": "floor-stop",
                "free_bytes": 4000000000, "private_bytes": 9000000000, "floor_bytes": 4294967296}))
            row = bench.analyse_scene(root, "remake-dev4", bench.SCENES["remake-dev4"])
            self.assertIn("4 GiB", row["measurement_error"])
            self.assertEqual(row["floor_stop"]["private_bytes"], 9000000000)

    def test_missing_requested_scene_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "_spec.json").write_text('{"runs": [{"name": "stock-bespin"}]}')
            self.assertFalse(bench.analyse(root, {}))
            result = json.loads((root / "results.json").read_text())
            self.assertFalse(result["valid"])
            self.assertEqual(result["scenes"][0]["scene"], "stock-bespin")

    def test_mod_chain_must_be_explicit_and_hosts_are_bounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                bench.write_spec(Path(tmp) / "spec.json", ["remake-menu"], "audio")
        with self.assertRaises(SystemExit):
            bench.run(argparse.Namespace(machine=["local"]))

    def test_nonfinite_values_are_missing(self):
        self.assertEqual(bench.column([{"x": "nan"}, {"x": "inf"}, {"x": "12"}], "x"), [12])
        self.assertIsNone(bench.stats([])["p95"])

    def test_raw_scaling_has_exact_added_counts_and_separate_roots(self):
        self.assertEqual(len(bench.select("raw-late-fleet")), 3)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "spec.json"
            with self.assertRaises(SystemExit):
                bench.write_spec(path, bench.select("raw-late-fleet"), "audio", mod_root="wrong-mod")
            bench.write_spec(path, bench.select("raw-late-fleet"), "free", raw_mod_root="raw-fixture", prepare=True)
            data = json.loads(path.read_text())
            for size, run in zip((15, 30, 60), data["runs"]):
                self.assertEqual(run["benchmark"]["window"], [1200, 3500])
                self.assertEqual(run["benchmark"]["ticks"], 1)
                self.assertEqual(len(run["benchmark"]["fleets"]["1"]), size)
                self.assertEqual(len(run["benchmark"]["fleets"]["2"]), size)
                self.assertEqual(run["args"][run["args"].index("--eawr-mod-root") + 1], "raw-fixture")
                self.assertNotIn("wrong-mod", run["args"])

    def test_raw_without_observed_combat_is_not_a_fleet_fight_measurement(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            name = "raw-late-fleet15"
            fleet = [dict(player=player, type=kind) for player, names in bench.SCENES[name]["fleets"].items()
                     for kind in names]
            report = root / f"{name}.json"
            report.write_text(json.dumps(dict(live_session=dict(start_fleet=fleet, free_starting_forces=[]))))
            (root / "exit-codes.txt").write_text(f"{name} 0\n")
            trace = root / f"{name}.csv"
            trace.write_text("tick,wall_frame_ms,projectiles\n1200,20,0\n3500,30,0\n")
            self.assertIn("engagement", bench.analyse_scene(root, name, bench.SCENES[name])["measurement_error"])
            trace.write_text("tick,wall_frame_ms,projectiles\n1200,20,1\n3500,30,1\n")
            self.assertNotIn("measurement_error", bench.analyse_scene(root, name, bench.SCENES[name]))
            report.write_text(json.dumps(dict(live_session=dict(start_fleet=fleet[:-1], free_starting_forces=[]))))
            self.assertIn("fleet definition", bench.analyse_scene(root, name, bench.SCENES[name])["measurement_error"])

    def test_completed_scene_on_input_desktop_is_invalid(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "exit-codes.txt").write_text("stock-bespin 0\n")
            (root / "stock-bespin.csv").write_text("tick,wall_frame_ms,projectiles\n600,20,1\n2300,30,1\n")
            receipt = root / "rig.json"
            receipt.write_text('{"machine":"fixture", "privateDesktop":false, "timedOut":false}')
            self.assertFalse(bench.analyse(root, {}))
            self.assertIn("private desktop", json.loads((root / "results.json").read_text())["protocol_errors"][0])
            receipt.write_text('{"machine":"fixture", "privateDesktop":true, "timedOut":false}')
            self.assertTrue(bench.analyse(root, {}))

    def test_owner_dev4_and_render_thread_protocol(self):
        def without_empire_fleet(args):
            result = list(args)
            index = next(i for i in range(len(result) - 1)
                         if result[i] == "--eawr-skirmish-fleet" and result[i + 1].startswith("2:"))
            del result[index:index + 2]
            return result
        common = without_empire_fleet(bench.SCENES["remake-dev4"]["args"])
        for count in (1, 2, 4):
            self.assertEqual(without_empire_fleet(bench.SCENES[f"remake-dev{count}"]["args"]), common)
            self.assertEqual(bench.SCENES[f"remake-dev{count}"]["ticks"], 1500)
            self.assertEqual(bench.SCENES[f"remake-dev{count}"]["window"], (600, 1400))
            self.assertTrue(bench.SCENES[f"remake-dev{count}"]["fixed_window"])
        scene = bench.SCENES["remake-dev4"]
        self.assertEqual(scene["ticks"], 1500)
        self.assertEqual(scene["args"][scene["args"].index("--eawr-profile") + 1], "remake")
        self.assertEqual(scene["args"][scene["args"].index("--eawr-live-ticks") + 1], "1500")
        self.assertEqual(scene["args"][scene["args"].index("--eawr-live-capture-ticks") + 1], "0")
        self.assertEqual(scene["fleets"][1], bench.TARGETS)
        self.assertEqual(scene["fleets"][2], bench.FIGHT * 4)
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "spec.json"
            bench.write_spec(path, ["remake-dev4"], "free", mod_root="fixture", prepare=True)
            run = json.loads(path.read_text())["runs"][0]
            self.assertEqual(run["engineArgs"][-2:], ["--render-thread", "separate"])
            self.assertEqual(run["benchmark"]["render_thread"], "separate")
            with self.assertRaisesRegex(ValueError, "owner census engagement"):
                bench.write_spec(path, ["remake-dev4"], "free", mod_root="fixture",
                                 preparations={"remake-dev4": self.native_start()})
            native = self.native_start()
            live = native["live_session"]
            live["start_map"] = "data/art/maps/korriban.ted"
            live["start_markers"] = [dict(player=p, use="spawn", position=[103.177246, -720.166992, 0])
                                     for p in (1, 2)]
            live["start_fleet"] = [dict(entity=e, player=1 if e <= 9 else 2, type="fixture",
                                        position=[103.177246, -720.166992, 0])
                                   for e in list(range(2, 10)) + list(range(11, 18))]
            bench.write_spec(path, ["remake-dev4"], "free", mod_root="fixture",
                             preparations={"remake-dev4": native})
            run = json.loads(path.read_text())["runs"][0]
            self.assertEqual(run["driverArgs"]["freeFloorGiB"], 4)
            orders = [run["args"][i + 1] for i, value in enumerate(run["args"]) if value == "--eawr-live-order"]
            self.assertEqual(orders, ["1:attack_move:2+3+4+5+6+7+8+9@103.177246,-720.166992,0",
                                      "1:attack_move:11+12+13+14+15+16+17@103.177246,-720.166992,0"])
            camera = bench.ET.parse(Path(tmp) / "engagement/remake-dev4.xml").getroot()
            self.assertAlmostEqual(float(camera.find("initial").attrib["target_x"]), 103.177246)
            self.assertAlmostEqual(float(camera.find("initial").attrib["target_y"]), -720.166992)

    def test_separate_mode_requires_engine_log_proof(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "_spec.json").write_text(json.dumps({"runs": [{"name": "stock-bespin",
                "benchmark": {"render_thread": "separate"}}]}))
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertFalse(row["render_thread_observed"])
            (root / "stock-bespin.log").write_text("WARNING: The separate rendering thread feature is experimental")
            row = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertTrue(row["render_thread_observed"])

    def test_engagement_commands_both_owners_and_fixes_camera_at_native_midpoint(self):
        native = self.native_start()
        recipe = bench.engagement_recipe(native)
        self.assertEqual(recipe["center"], [20, 10])
        self.assertEqual(recipe["orders"], ["1:attack_move:7+91@20.000000,10.000000,0",
                                             "1:attack_move:305@20.000000,10.000000,0"])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "spec.json"
            bench.write_spec(path, ["stock-bespin"], "free", preparations={"stock-bespin": native})
            run = json.loads(path.read_text())["runs"][0]
            args = run["args"]
            self.assertEqual(args[args.index("--eawr-diagnostic-vsync") + 1], "0")
            self.assertEqual(args[args.index("--eawr-diagnostic-max-fps") + 1], "0")
            self.assertNotIn("--eawr-live-audio-pace", args)
            self.assertEqual(run["engineArgs"], ["--max-fps", "0", "--render-thread", "separate", "--script", "{in}/observer/menu-observer.gd"])
            self.assertEqual(run["benchmark"]["observer"], "buffered-post-draw-v2")
            observer = (Path(tmp) / "observer/menu-observer.gd").read_text()
            self.assertNotIn(".flush()", observer)
            self.assertIn("root.tree_exiting.connect(write_trace)", observer)
            camera = bench.ET.parse(Path(tmp) / "engagement/stock-bespin.xml").getroot()
            self.assertEqual(camera.attrib["map_sha256"], "a" * 64)
            self.assertEqual(float(camera.find("initial").attrib["target_x"]), 20)
            self.assertEqual(float(camera.find("initial").attrib["target_y"]), 10)
        native["live_session"]["start_seed"] = 99
        with self.assertRaises(ValueError):
            bench.engagement_recipe(native)

    def test_free_protocol_rejects_caps_and_an_unaccepted_start(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            native = self.native_start()
            identity = bench.start_identity(native)
            (root / "exit-codes.txt").write_text("stock-bespin 0\n")
            (root / "stock-bespin.json").write_text(json.dumps(native))
            (root / "_spec.json").write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=dict(
                pacing="free", engagement=dict(start_identity=identity)))])))
            trace = root / "stock-bespin.csv"
            header = "tick,wall_frame_ms,projectiles,engine_max_fps,vsync_mode\n"
            trace.write_text(header + "600,20,1,60,1\n700,20,1,60,1\n2300,30,1,60,1\n")
            self.assertIn("uncapped", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            trace.write_text(header + "600,20,1,0,0\n700,20,1,0,0\n2300,30,1,0,0\n")
            observer = root / "stock-bespin.observer.csv"
            observer.write_text("frame,engine_max_fps,vsync_mode\n0,0,0\n1,60,0\n2,0,0\n")
            self.assertIn("uncapped", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            observer.write_text("frame,engine_max_fps,vsync_mode\n0,0,0\n1,0,0\n2,0,0\n")
            # Native process-scope counter precedes the existing pre-draw override.
            trace.write_text(header + "600,20,1,120,0\n700,20,1,120,0\n2300,30,1,120,0\n")
            self.assertNotIn("measurement_error", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"]))
            native["live_session"]["start_fleet"][0]["entity"] = 92
            (root / "stock-bespin.json").write_text(json.dumps(native))
            self.assertIn("differs", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])

    def test_flushing_observer_protocol_is_invalid_even_with_completed_cap_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "exit-codes.txt").write_text("stock-bespin 0\n")
            (root / "stock-bespin.csv").write_text("tick,wall_frame_ms,projectiles\n600,20,1\n700,20,1\n2300,30,1\n")
            (root / "stock-bespin.observer.csv").write_text("frame,engine_max_fps,vsync_mode\n0,0,0\n1,0,0\n2,0,0\n")
            spec = root / "_spec.json"
            definition = dict(phase="measure", pacing="free")
            spec.write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=definition)])))
            self.assertIn("periodic observer I/O", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            definition["observer"] = "buffered-post-draw-v2"
            spec.write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=definition)])))
            self.assertNotIn("measurement_error", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"]))
            definition["log_mode"] = "pipe"
            spec.write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=definition)])))
            self.assertIn("collector", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            (root / "stock-bespin.log").write_bytes(b"all output\n")
            receipt = dict(bytes=11, sha256=hashlib.sha256(b"all output\n").hexdigest(), exit_code=0, suppressed_bytes=0)
            (root / "stock-bespin.collector.json").write_text(json.dumps(receipt))
            self.assertNotIn("measurement_error", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"]))
            (root / "stock-bespin.proc.csv").write_text("elapsed_ms,private_bytes,cpu_seconds,log_bytes\n0,1000,0,0\n1000,1000,1,0\n")
            stale = bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])
            self.assertNotIn("measurement_error", stale)
            self.assertIn("stayed zero", stale["log_rate_error"])
            self.assertNotIn("log_growth_post_startup", stale)
            (root / "stock-bespin.log").write_bytes(b"truncated")
            self.assertIn("collector", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            definition["log_mode"] = "nul"
            spec.write_text(json.dumps(dict(runs=[dict(name="stock-bespin", benchmark=definition)])))
            self.assertIn("NUL control", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"])["measurement_error"])
            (root / "stock-bespin.log").unlink()
            self.assertNotIn("measurement_error", bench.analyse_scene(root, "stock-bespin", bench.SCENES["stock-bespin"]))


if __name__ == "__main__":
    unittest.main()
