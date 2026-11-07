"""Contract tests for diagnostic pairing, provenance and standalone output."""
import argparse
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("drawtype", ROOT / "tools/performance/drawtype_profile.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)
spec = importlib.util.spec_from_file_location("bench", ROOT / "tools/performance/remake_bench.py")
bench = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bench)


class DrawtypeTests(unittest.TestCase):
    def test_impostor_matrix_uses_matched_control_and_repeats(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "spec.json"
            bench.write_spec(path, ["stock-menu"], "free", drawtype_ablate="impostors", drawtype_repeats=2)
            runs = json.loads(path.read_text())["runs"]
            self.assertEqual(len(runs), 4)
            for repeat in (1, 2):
                self.assertEqual({r["benchmark"]["drawtype_ablate"] for r in runs if r["benchmark"]["repeat"] == repeat},
                                 {"fighter-impostor-control", "fighter-impostor-008"})
            for run in runs:
                self.assertEqual(run["engineArgs"][:4], ["--max-fps", "0", "--render-thread", "separate"])

    def test_fighter_matrix_keeps_matched_clocks_and_repeats(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "spec.json"
            bench.write_spec(path, ["stock-menu"], "free", drawtype_ablate="fighters", drawtype_repeats=2)
            runs = json.loads(path.read_text())["runs"]
            self.assertEqual(len(runs), 8)
            for repeat in (1, 2):
                self.assertEqual({r["benchmark"]["drawtype_ablate"] for r in runs if r["benchmark"]["repeat"] == repeat},
                                 {"fighter-control", "fighter-hull", "fighter-all", "fighter-shadows"})
            for run in runs:
                self.assertEqual(run["engineArgs"][:4], ["--max-fps", "0", "--render-thread", "separate"])
                self.assertIn("--eawr-diagnostic-max-fps", run["args"])

    def test_bands_generate_matched_repeats_and_export_all_heatmap_columns(self):
        scene = next(n for n, s in bench.SCENES.items() if s["kind"] == "attract" and not s["mod"])
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            path = out / "spec.json"
            bench.write_spec(path, [scene], "free", drawtype_ablate="bands", drawtype_repeats=2)
            runs = json.loads(path.read_text())["runs"]
            self.assertEqual(len(runs), 26)
            self.assertEqual({r["benchmark"]["drawtype_ablate"] for r in runs}, {"none", *profile.EXPERIMENTS})
            for run in runs:
                self.assertEqual(run["args"][-2:], ["--eawr-drawtype-ablate", run["benchmark"]["drawtype_ablate"]])
            data = self.receipt()
            for repeat in (1, 2):
                for category in profile.EXPERIMENTS:
                    row = copy.deepcopy(data["scenes"][0])
                    row.update(drawtype_ablate=category, repeat=repeat, draws=dict(median=90))
                    data["scenes"].append(row)
            for row in data["scenes"]:
                if row["drawtype_ablate"] == "none":
                    row["producer_samples"] = [dict(diagnostic=dict(eligible_units=6, compatible_groups=1, potential_colour_saving=20))]
            result = profile.summarize([data])
            table = {r["type"]: r for r in result["scenes"]["stock"]["table"]}
            self.assertEqual(table["hardpoint-band-005"]["delta"]["draws"]["median"], 10)
            self.assertEqual(result["scenes"]["stock"]["retained_eligible_diagnostics"]["potential_colour_saving"]["median"], 20)
            profile.write_outputs(result, out)
            svg = (out / "drawtype-heatmap.svg").read_text()
            for category in profile.EXPERIMENTS:
                self.assertIn(category, svg)
            data["scenes"].pop()
            with self.assertRaises(ValueError):
                profile.summarize([data])

    def test_nonseparable_producers_still_report_known_geometry(self):
        data = self.receipt()
        for row in data["scenes"]:
            if row["drawtype_ablate"] == "none":
                row["producer_samples"] = [dict(roles={
                    "map": dict(retained=1, retained_surfaces=2, potential_shadow_surfaces=1),
                    "sky": dict(retained=2, retained_surfaces=3, potential_shadow_surfaces=0),
                    "unknown": dict(retained=1, retained_surfaces=4, potential_shadow_surfaces=0)})]
        table = {r["type"]: r for r in profile.summarize([data])["scenes"]["stock"]["table"]}
        self.assertEqual(table["terrain/env"]["status"], "not separable")
        self.assertEqual(table["terrain/env"]["producer"]["retained"]["median"], 3)
        self.assertEqual(table["terrain/env"]["producer_estimate_share_percent"]["median"], 60)
        self.assertEqual(profile.summarize([data])["scenes"]["stock"]["top3_by_producer_estimated_share"][0], "terrain/env")
        self.assertIsNone(table["terrain/env"]["producer"]["retained_triangles"])
        self.assertEqual(table["other"]["producer"]["retained_surfaces"]["median"], 4)
        self.assertIsNone(table["projectile"]["producer"]["retained"])
        self.assertEqual(table["shadows"]["producer"]["draw_estimate_one_colour_one_shadow"]["median"], 1)
        self.assertEqual(table["shadows"]["producer_estimate_share_percent"]["median"], 10)
        self.assertIsNone(table["shadows"]["producer"]["retained_triangles"])

    def test_zero_gpu_timer_pairs_remain_unavailable(self):
        data = self.receipt()
        for row in data["scenes"]:
            row["gpu_ms"] = dict(median=0)
        hull = profile.summarize([data])["scenes"]["stock"]["table"][0]
        self.assertIsNone(hull["delta"]["gpu_ms"])
        self.assertIsNotNone(hull["delta"]["render_cpu_ms"])

    def test_rig_retains_prelaunch_engine_pins_and_never_reuses_an_old_run(self):
        pins = {"files": {"engine.exe": "c" * 64, "engine_console.exe": "d" * 64}}
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            old = out / "old-run"
            old.mkdir()
            (old / "rig.json").write_text("{}")
            args = argparse.Namespace(timeout=1, queue_timeout=1, viewer_artifact="artifact.json", machine=["synthetic"])
            with patch.object(bench, "capture_engine_pins", return_value=pins), patch.object(bench.subprocess, "run", return_value=argparse.Namespace(returncode=1)):
                code, directory = bench.invoke_rig(args, out / "spec.json", out)
                self.assertEqual(code, 1)
                self.assertIsNone(directory)
                self.assertFalse((old / "engine-pins.json").exists())
            def completed(*args, **kwargs):
                new = out / "new-run"
                new.mkdir()
                (new / "rig.json").write_text("{}")
                return argparse.Namespace(returncode=0)
            with patch.object(bench, "capture_engine_pins", return_value=pins), patch.object(bench.subprocess, "run", side_effect=completed):
                code, directory = bench.invoke_rig(args, out / "spec.json", out)
                self.assertEqual(code, 0)
                self.assertEqual(directory.name, "new-run")
                self.assertEqual(json.loads((directory / "engine-pins.json").read_text()), pins)
                self.assertEqual(json.loads((directory / "bench-invocation.json").read_text()), {"exit_code": 0})

    def test_native_counter_frames_join_producers_to_the_tick_window_and_prove_the_flag(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            name = "stock-bespin--hull--1"
            definition = dict(name=name, benchmark=dict(drawtype_ablate="hull", repeat=1, source_scene="stock-bespin",
                render_thread="separate", phase="measure", observer="buffered-post-draw-v2", log_mode="file"))
            (directory / "_spec.json").write_text(json.dumps(dict(runs=[definition])))
            (directory / "exit-codes.txt").write_text(name + " 0\n")
            (directory / (name + ".csv")).write_text("tick,wall_frame_ms,projectiles,render_counter_frame\n500,20,1,60\n600,20,1,120\n2300,20,1,180\n")
            samples = [dict(frame=frame, ablation="hull", roles={"hull": dict(retained=count)})
                       for frame, count in ((60, 999), (120, 2), (180, 3))]
            banner = "Godot Engine vSYNTHETIC\nThe separate rendering thread feature is experimental\n"
            log = directory / (name + ".log")
            log.write_text(banner + "\n".join("EAWR_RENDER_CATEGORIES " + json.dumps(s) for s in samples))
            row = bench.analyse_scene(directory, name, bench.SCENES["stock-bespin"])
            self.assertTrue(row["ablation_observed"])
            self.assertEqual([s["frame"] for s in row["producer_samples"]], [120, 180])
            samples[1]["ablation"] = "none"
            log.write_text(banner + "\n".join("EAWR_RENDER_CATEGORIES " + json.dumps(s) for s in samples))
            row = bench.analyse_scene(directory, name, bench.SCENES["stock-bespin"])
            self.assertFalse(row["ablation_observed"])
            self.assertIn("ablation", row["measurement_error"])

    def test_render_errors_keep_the_export_red_diagnostic(self):
        data = self.receipt()
        data["scenes"][0]["error_markers_total"] = 1
        result = profile.summarize([data])
        self.assertEqual(result["diagnostic_status"], "RED diagnostic")
        self.assertEqual(result["render_error_runs"][0]["errors"], 1)

    def test_terminal_missing_memory_is_reported_without_dropping_frame_or_cpu_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / "case.proc.csv").write_text("elapsed_ms,private_bytes,working_set_bytes,cpu_seconds,log_bytes\n0,536870912,100,1,0\n2000,,,2,10\n")
            result = bench.process_stats(directory, "case", 0)
            self.assertEqual(result["process_memory_samples_missing"], 1)
            self.assertEqual(result["private_gib_peak"], 0.5)
            self.assertEqual(result["process_cores_tail"], 0.5)

    def receipt(self):
        rows = []
        for repeat in (1, 2):
            for category, draws, cpu in (("none", 100, 10), ("hull", 60, 7), ("hardpoint", 110, 11)):
                rows.append(dict(source_scene="stock", drawtype_ablate=category, repeat=repeat,
                                 render_thread_observed=True, engine_build="synthetic engine", draws=dict(median=draws),
                                 render_cpu_ms=dict(median=cpu), frame_ms=dict(median=cpu*2)))
        return dict(valid=True, meta=dict(machine="synthetic-test-host", exit_code=0, pacing="free", viewer="test-build", viewer_dll_sha256="test-dll", engine_gui_sha256="c" * 64), scenes=rows)

    def test_pair_deltas_and_negative_results(self):
        result = profile.summarize([self.receipt()])["scenes"]["stock"]
        hull, hp = result["table"][:2]
        self.assertEqual(hull["delta"]["draws"]["median"], 40)
        self.assertEqual(hp["delta"]["draws"]["median"], -10)
        self.assertEqual(hull["draw_delta_percent"]["median"], 40)
        self.assertEqual(result["top3_by_draw_delta"], ["hull", "hardpoint"])
        self.assertEqual(result["table"][-1]["status"], "not separable")

    def test_estimated_share_uses_all_roles_and_is_distinct_from_measured_delta(self):
        data = self.receipt()
        for row in data["scenes"]:
            if row["drawtype_ablate"] == "none":
                row["producer_samples"] = [dict(roles={
                    "hull": dict(retained=2, retained_surfaces=3, potential_shadow_surfaces=3, retained_triangles=40, material_incidence=3),
                    "unknown": dict(retained=1, retained_surfaces=4, potential_shadow_surfaces=0)})]
        hull = profile.summarize([data])["scenes"]["stock"]["table"][0]
        self.assertEqual(hull["producer_estimate_share_percent"]["median"], 60)
        self.assertEqual(hull["draw_delta_percent"]["median"], 40)
        self.assertEqual(hull["producer"]["draw_estimate_one_colour_one_shadow"]["median"], 6)

    def test_reject_unpaired_invalid_and_mixed_builds(self):
        data = self.receipt()
        for mutate in (lambda d: d.update(valid=False),
                       lambda d: d["scenes"][0].update(render_thread_observed=False),
                       lambda d: d["scenes"].pop(0),
                       lambda d: d["scenes"].pop(1),
                       lambda d: d["meta"].update(pacing="audio"),
                       lambda d: d["meta"].update(exit_code=1)):
            bad = copy.deepcopy(data)
            mutate(bad)
            with self.assertRaises(ValueError):
                profile.summarize([bad])
        other = copy.deepcopy(data)
        other["meta"]["viewer"] = "another-build"
        with self.assertRaises(ValueError):
            profile.summarize([data, other])
        other = copy.deepcopy(data)
        other["meta"]["engine_gui_sha256"] = "d" * 64
        with self.assertRaises(ValueError):
            profile.summarize([data, other])

    def test_outputs_are_standalone_and_escape_labels(self):
        data = self.receipt()
        for row in data["scenes"]:
            row["source_scene"] = "stock<synthetic>"
        with tempfile.TemporaryDirectory() as temporary:
            out = Path(temporary)
            profile.write_outputs(profile.summarize([data]), out)
            svg = (out / "drawtype-heatmap.svg").read_text()
            self.assertIn("stock&lt;synthetic&gt;", svg)
            self.assertIn("non-additive", (out / "drawtype.md").read_text())
            self.assertTrue((out / "drawtype.json").is_file())

    def test_bench_generates_matched_rotated_repeats_without_changing_default(self):
        scene = next(n for n, s in bench.SCENES.items() if s["kind"] == "attract" and not s["mod"])
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "spec.json"
            bench.write_spec(path, [scene], "free")
            original = json.loads(path.read_text())["runs"][0]
            self.assertNotIn("--eawr-drawtype-ablate", original["args"])
            bench.write_spec(path, [scene], "free", drawtype_ablate="hull", drawtype_repeats=2)
            runs = json.loads(path.read_text())["runs"]
            self.assertEqual(len(runs), 4)
            for run in [original, *runs]:
                self.assertEqual(run["args"][run["args"].index("--eawr-settings-file") + 1],
                                 "{in}/settings/uncapped.txt")
            self.assertEqual([r["benchmark"]["drawtype_ablate"] for r in runs], ["none", "hull", "hull", "none"])
            for run in runs:
                self.assertEqual(run["benchmark"]["source_scene"], scene)
                self.assertIn("--render-thread", run["engineArgs"])
                self.assertIn("--bench-label=" + run["name"], run["args"])
                self.assertEqual(run["benchmark"]["observer"], original["benchmark"]["observer"])
