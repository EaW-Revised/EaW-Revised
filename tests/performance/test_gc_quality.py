"""Contract tests for tools/gc_quality: statistics, decision-log parsing, the comparison report, the sweep plan and the
corpus dry-run comparison. Everything runs on synthetic logs; no game data and no build are needed."""
import gzip
import json
import math
import random
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from gc_quality import compare, corpus_upgrade, logs, metrics, runner, stats, techup  # noqa: E402


def synth_run(arm, seed, ticks=54000, shift=0, stub=False, retarget=False, extra_latency=0, profile="stock", probe="none",
              tech_late=False, refusals="none"):
    """A synthetic decision log. The same seed gives the same decisions in both arms; `shift` delays them (the async
    delay), `retarget` changes every target, `extra_latency` lengthens the reaction to enemy arrivals."""
    rng = random.Random(seed * 7919)
    meta = {"r": "meta", "schema": logs.SCHEMA, "seed": seed, "arm": arm, "arm_effective": "reference" if stub or arm == "reference" else "async",
            "stub": stub, "profile": profile, "campaign": "c", "ticks": ticks, "probe": probe, "probe_tick": 3000, "human": 1,
            "workers": 1, "sample_period": 300, "ai": [{"p": 2, "type": "BasicEmpire", "faction": "Empire"},
                                                         {"p": 3, "type": "BasicRebel", "faction": "Rebel"}]}
    run = logs.Run(meta=meta)
    for player in (2, 3):
        t = 100
        for i in range(120):
            t += rng.randint(120, 700)
            if t > ticks - 800:
                break
            kind = rng.choice(("move", "move", "build"))
            tc = rng.choice(("enemy", "neutral", "own", "own"))
            target = 100 + rng.randint(0, 15)
            if retarget:
                target += 1
            if kind == "move":
                run.events.append({"r": "ev", "t": t + shift, "k": "movement_begin", "o": player, "s": 5000 + i, "from": 100 + i % 7,
                                   "to": target, "tc": tc, "n": 3})
            else:
                run.events.append({"r": "ev", "t": t + shift, "k": "production_started", "o": player, "s": 100 + i % 7, "ty": 7, "sq": i})
            if i % 9 == 0:   # an enemy arrival at one of the player's planets, answered by an order on that planet
                at = 100 + i % 7
                arrival = t + 5
                run.events.append({"r": "ev", "t": arrival + shift, "k": "movement_finished", "o": 1, "s": 9000 + i, "at": at, "po": player})
                run.events.append({"r": "ev", "t": arrival + shift + 8 + extra_latency, "k": "movement_begin", "o": player, "s": 6000 + i,
                                   "from": at, "to": 105, "tc": "own", "n": 2})
    run.events.sort(key=lambda e: e["t"])
    for t in range(300, ticks + 1, 300):
        for player in (2, 3):
            tech = 1 + (t >= (30000 if tech_late else 15000)) * 2 + (t >= 40000)
            run.samples.append({"r": "smp", "t": t, "p": player, "cr": 1000 + t // 10, "tech": tech, "pl": 3 + t // 9000, "fl": 4})
    for t in (900, ticks):
        for player in (2, 3):
            run.goals.append({"r": "goals", "t": t, "line": f"player {player} Basic: 4 goals, 2 movement rows; Expand n=2 active=1 planned=0 running=0 desire<=3; "
                                                            f"Attack n=2 active=1 planned=0 running=0 desire<=2; refusals: {refusals}"})
    run.end = {"r": "end", "ticks": ticks, "hashes": ticks, "hash_fnv": seed, "ai_orders": len(run.events), "ai_rejected": 0, "wall_s": 12.5}
    return run


class StatsTests(unittest.TestCase):
    def test_ks_and_percentiles(self):
        d, p = stats.ks_test([1, 2, 3, 4, 5], [3, 4, 5, 6, 7])
        self.assertAlmostEqual(d, 0.4)
        self.assertGreater(p, 0.5)
        d, p = stats.ks_test(list(range(50)), [x + 100 for x in range(50)])
        self.assertEqual(d, 1.0)
        self.assertLess(p, 1e-6)
        self.assertEqual(stats.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 95), 10)
        self.assertEqual(stats.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 50), 5)

    def test_multiplicity_and_intervals(self):
        self.assertEqual([round(x, 6) for x in stats.holm([0.01, 0.04, 0.03])], [0.03, 0.06, 0.06])
        low, high = stats.wilson(8, 10)
        self.assertAlmostEqual(low, 0.4902, places=3)
        self.assertAlmostEqual(high, 0.9433, places=3)
        low, high = stats.poisson_interval(10)
        self.assertAlmostEqual(low, 4.7954, places=3)
        self.assertAlmostEqual(high, 18.3904, places=3)
        self.assertEqual(stats.poisson_interval(0)[0], 0.0)

    def test_median_shift_and_splits(self):
        self.assertAlmostEqual(stats.relative_shift([10, 10, 10], [11, 11, 11]), 0.1)
        self.assertEqual(stats.relative_shift([0, 0], [0, 0]), 0.0)
        pairs = stats.balanced_splits(4, 2, 100, seed=1)
        self.assertEqual(len(pairs), 6)
        for first, second in pairs:
            self.assertFalse(set(first) & set(second))
        self.assertEqual(len(stats.balanced_splits(12, 6, 50, seed=1)), 50)
        self.assertEqual(stats.balanced_splits(3, 2, 10, seed=1), [])


class LogTests(unittest.TestCase):
    def test_extract_log_rows_and_round_trip(self):
        run = synth_run("reference", 3, ticks=6000)
        rows = [run.meta, *run.events, *run.samples, *run.goals, run.end]
        text = "noise line\n" + "\n".join("GCQ " + json.dumps(r) for r in rows) + "\n[test 2/3] trailing\n"
        # a second run and a cut line (no end row) in the same log
        cut = synth_run("async", 3, ticks=6000, stub=True)
        text += "GCQ " + json.dumps(cut.meta) + "\nGCQ " + json.dumps(cut.events[0])[:20] + "\n"
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "t.log"
            path.write_text(text, encoding="utf-8")
            runs = logs.extract_runs(path)
            self.assertEqual(len(runs), 2)
            self.assertTrue(runs[0].complete)
            self.assertFalse(runs[1].complete)
            self.assertEqual(len(runs[0].events), len(run.events))
            stored = logs.write_run(Path(tmp) / "out", runs[0])
            self.assertEqual(stored.name, "stock-reference-s003-t6000.ndjson.gz")
            again = logs.read_ndjson(stored)[0]
            self.assertEqual(again.events, runs[0].events)
            self.assertEqual(logs.read_any(stored)[0].key, runs[0].key)
            with gzip.open(stored, "rt") as handle:
                self.assertEqual(json.loads(handle.readline())["r"], "meta")

    def test_unknown_schema_is_refused(self):
        with self.assertRaises(ValueError):
            logs.parse_rows([{"r": "meta", "schema": "gcq-0", "seed": 1, "arm": "reference"}])


class MetricTests(unittest.TestCase):
    def test_latency_triggers_and_windows(self):
        run = synth_run("reference", 1, ticks=54000)
        rows, grouped = metrics.reaction_latencies(run)
        self.assertTrue(rows)
        self.assertTrue(all(planet == 8 and first <= 8 for _, first, planet in rows if planet is not None))
        self.assertEqual(set(grouped), {"enemy_arrival"})
        windows = metrics.window_rows(run)
        self.assertEqual({w["window"] for w in windows}, {0, 1, 2})
        self.assertTrue(all(w["tech"] is not None for w in windows))
        self.assertEqual(metrics.tech_times(run)[2]["tech3"], 15000)

    def test_refusal_parse(self):
        run = synth_run("reference", 1, ticks=3000, refusals="no_target=4, stale=1")
        self.assertEqual(metrics.refusal_totals(run), {"no_target": 8, "stale": 2})


class CompareTests(unittest.TestCase):
    def runs(self, **async_kwargs):
        seeds = (1, 2, 3, 4)
        reference = [synth_run("reference", s) for s in seeds] + [synth_run("reference", s) for s in (101, 102, 103, 104)]
        asyn = [synth_run("async", s, **async_kwargs) for s in seeds]
        return reference + asyn

    def status(self, report, fragment):
        return [c.status for c in report.checks if fragment in c.name]

    def test_stub_run_is_not_run_not_pass(self):
        report = compare.build_report(self.runs(stub=True))
        self.assertEqual(report.verdict, compare.NOT_RUN, compare.render(report))
        self.assertIn("**Verdict: NOT-RUN**", compare.render(report))
        self.assertTrue(any("stub" in w for w in report.warnings))
        self.assertEqual(self.status(report, "early-window identity"), [compare.PASS])
        self.assertEqual(self.status(report, "tick-hash digest"), [compare.PASS])
        runs = self.runs(stub=True)
        runs[-1].end["hash_fnv"] += 1   # one stub seed no longer reproduces its reference
        self.assertEqual(self.status(compare.build_report(runs), "tick-hash digest"), [compare.FAIL])

    def real_runs(self, **kwargs):
        """A genuine second arm (not a stub) with the design's snapshot delay."""
        return self.runs(shift=36, extra_latency=36, **kwargs)

    def test_real_run_with_matching_behaviour_still_passes(self):
        report = compare.build_report(self.real_runs())
        self.assertEqual(report.verdict, compare.PASS, compare.render(report))
        self.assertEqual(report.not_run, [])
        self.assertEqual(report.compared, 1)

    def test_reference_only_is_incomplete(self):
        reference_only = [synth_run("reference", s) for s in (1, 2, 3, 4)]
        report = compare.build_report(reference_only)
        self.assertEqual(report.verdict, compare.INCOMPLETE, compare.render(report))
        self.assertEqual(self.status(report, "comparison"), [compare.NA])
        self.assertEqual(compare.build_report([]).verdict, compare.INCOMPLETE)

    def test_dead_async_arms_are_not_pass(self):
        runs = self.real_runs()
        for run in runs:
            if run.meta["arm"] == "async":
                run.events = []   # no orders, no answered triggers
        report = compare.build_report(runs)
        self.assertNotEqual(report.verdict, compare.PASS, compare.render(report))

    def test_run_too_short_for_the_tech_gate_is_incomplete(self):
        seeds = (1, 2, 3, 4)
        runs = [synth_run("reference", s, ticks=20000) for s in seeds] + [synth_run("async", s, ticks=20000, shift=36, extra_latency=36) for s in seeds]
        report = compare.build_report(runs)
        self.assertEqual(self.status(report, "tech 3"), [compare.NA])
        self.assertEqual(report.verdict, compare.INCOMPLETE, compare.render(report))

    def test_smoke_run_is_incomplete(self):
        runs = [synth_run("reference", 1, ticks=2999), synth_run("async", 1, ticks=2999, shift=36)]
        self.assertEqual(compare.build_report(runs).verdict, compare.INCOMPLETE)

    def test_unreadable_refusal_counts_fail_loudly(self):
        for broken in ("no_target=x", "no_target", "=4"):
            run = synth_run("reference", 1, ticks=3000, refusals=broken)
            with self.assertRaises(ValueError, msg=broken):
                metrics.refusal_totals(run)
        run = synth_run("reference", 1, ticks=3000)
        run.goals[0]["line"] = run.goals[0]["line"].replace("; refusals: none", "")   # the line format changed
        with self.assertRaises(ValueError):
            metrics.refusal_totals(run)
        runs = self.real_runs(refusals="stale=x")
        report = compare.build_report(runs)
        self.assertEqual(self.status(report, "refusal rate"), [compare.FAIL], compare.render(report))
        self.assertEqual(report.verdict, compare.FAIL)

    def test_no_goal_summaries_is_not_a_pass(self):
        runs = self.real_runs()
        for run in runs:
            run.goals = []
        report = compare.build_report(runs)
        self.assertEqual(self.status(report, "refusal rate"), [compare.NA])
        self.assertEqual(report.verdict, compare.INCOMPLETE)

    def test_exit_codes_follow_the_verdict(self):
        def exit_code(runs):
            with tempfile.TemporaryDirectory() as tmp:
                directory = Path(tmp) / "logs"
                for run in runs:
                    logs.write_run(directory, run)
                return compare.main([str(directory), "--out", str(Path(tmp) / "report.md")])
        self.assertEqual(exit_code(self.real_runs()), 0)
        self.assertEqual(exit_code(self.runs(stub=True)), 2)
        self.assertEqual(exit_code([synth_run("reference", 1)]), 2)
        self.assertEqual(exit_code(self.runs(shift=36, extra_latency=200, retarget=True)), 1)

    def test_delayed_async_inside_the_envelope_passes(self):
        report = compare.build_report(self.runs(shift=36, extra_latency=36))
        self.assertEqual(self.status(report, "tick shift"), [compare.PASS], compare.render(report))
        self.assertEqual(self.status(report, "latency added at p50"), [compare.PASS])
        self.assertEqual(self.status(report, "latency added at p95"), [compare.PASS])
        self.assertEqual(self.status(report, "latency floor"), [compare.PASS])
        self.assertEqual(report.verdict, compare.PASS, compare.render(report))

    def test_slow_retargeting_async_fails(self):
        report = compare.build_report(self.runs(shift=36, extra_latency=200, retarget=True, tech_late=True, refusals="stale=40"))
        self.assertEqual(self.status(report, "early-window identity"), [compare.FAIL])
        self.assertEqual(self.status(report, "latency added at p50"), [compare.FAIL])
        self.assertEqual(self.status(report, "tech 3"), [compare.FAIL])
        self.assertEqual(self.status(report, "refusal rate delta"), [compare.FAIL])
        self.assertEqual(report.verdict, compare.FAIL)
        text = compare.render(report)
        self.assertIn("**Verdict: FAIL**", text)

    def test_incomplete_runs_are_excluded_with_a_warning(self):
        runs = self.runs(stub=True)
        runs[-1].end = None
        report = compare.build_report(runs)
        self.assertTrue(any("no end row" in w for w in report.warnings))

    def test_noise_floor_needs_two_reference_groups(self):
        per_seed = {1: [1.0, 2.0, 3.0], 2: [1.0, 2.5, 3.0], 3: [1.5, 2.0, 3.5], 4: [1.0, 2.0, 4.0]}
        floor = compare.noise_floor(per_seed, 2, compare.Thresholds())
        self.assertIsNotNone(floor)
        self.assertEqual(floor["splits"], 6)
        self.assertIsNone(compare.noise_floor({1: [1.0, 2.0]}, 1, compare.Thresholds()))

    def test_probe_outcomes(self):
        run = synth_run("async", 1, ticks=7200, probe="near1")
        run.probes.append({"r": "probe", "t": 3000, "scenario": "near1", "issued": True, "target": 150, "fleets": 1})
        run.events += [{"r": "ev", "t": 3100, "k": "movement_finished", "o": 1, "s": 1, "at": 150, "po": 2},
                       {"r": "ev", "t": 3150, "k": "movement_begin", "o": 2, "s": 8, "from": 100, "to": 150, "tc": "own", "n": 4},
                       {"r": "ev", "t": 3200, "k": "auto_resolve_won", "s": 150, "v": 2}]
        outcome = metrics.probe_outcome(run)
        self.assertEqual(outcome["outcome"], "defended")
        self.assertEqual(outcome["first_response_latency"], 50)
        run.events[-1].update(k="auto_resolve_lost")
        self.assertEqual(metrics.probe_outcome(run)["outcome"], "lost")


class RunnerTests(unittest.TestCase):
    def test_plan_keeps_arms_of_a_seed_on_one_host(self):
        jobs = runner.make_plan("stock", [1, 2, 3, 4], [101, 102], 18000, ["near1"], [1], ["a", "b"])
        by_seed = {j["seed"]: j for j in jobs}
        self.assertEqual(set(by_seed[1]["tests"]), {"galactic_ai_quality_stock_reference_s1_t18000", "galactic_ai_quality_stock_async_s1_t18000"})
        self.assertEqual(by_seed[101]["tests"], ["galactic_ai_quality_stock_reference_s101_t18000"])
        self.assertEqual(len(by_seed[1001]["tests"]), 2)
        self.assertEqual({j["host"] for j in jobs}, {"a", "b"})
        with self.assertRaises(SystemExit):
            runner.make_plan("stock", [9], [], 18000, [], [], ["a"])

    def test_seed_ranges_and_command(self):
        self.assertEqual(runner.parse_seeds("1-3,7"), [1, 2, 3, 7])
        jobs = runner.make_plan("stock", [1], [], 18000, [], [], ["a"])
        command = runner.client_command(jobs[0], {"strict_probe_base": "f" * 40}, Path("out/x"))
        self.assertIn("--lock-timeout", command)
        self.assertEqual(command[command.index("--lock-timeout") + 1], "28800")
        self.assertEqual(command[command.index("--host") + 1], "a")
        self.assertIn("--strict-probe-base", command)
        self.assertTrue(command[command.index("--ctest-regex") + 1].startswith("^(galactic_ai_quality_stock_"))

    def test_ctest_outcome_parse(self):
        text = ("1/2 Test #1: galactic_ai_quality_stock_reference_s1_t300 ......   Passed   40.2 sec\n"
                "2/2 Test #2: galactic_ai_quality_raw_reference_s1_t300 ..........***Skipped   0.3 sec\n")
        self.assertEqual(runner.test_outcomes(text), {"galactic_ai_quality_stock_reference_s1_t300": "passed",
                                                      "galactic_ai_quality_raw_reference_s1_t300": "skipped"})

    def test_submit_and_collect_a_finished_job(self):
        # A fake client stands in for windows_build.py: it writes the job's test log (decision-log lines plus noise).
        with tempfile.TemporaryDirectory() as tmp:
            sweep = Path(tmp) / "sweep"
            sweep.mkdir()
            run_a, run_b = synth_run("reference", 1, ticks=3000), synth_run("async", 1, ticks=3000, stub=True)
            rows = [r for run in (run_a, run_b) for r in (run.meta, *run.events, *run.samples, *run.goals, run.end)]
            source = Path(tmp) / "payload.log"
            lines = ["configure ok"] + ["GCQ " + json.dumps(r) for r in rows]
            lines.append(" 1/2 Test #1: galactic_ai_quality_stock_reference_s1_t3000 ..... Passed 1.0 sec")
            source.write_text("\n".join(lines) + "\n", encoding="utf-8")
            fake = Path(tmp) / "fake_client.py"
            fake.write_text("\n".join([
                "import shutil, sys",
                "from pathlib import Path",
                "out = Path(sys.argv[sys.argv.index('--out') + 1])",
                "out.mkdir(parents=True, exist_ok=True)",
                f"shutil.copy({str(source)!r}, out / 'windows-msvc-ninja.test.log')", ""]), encoding="utf-8")
            plan = {"profile": "stock", "ticks": 3000, "strict_probe_base": None, "max_parallel": 1,
                    "jobs": runner.make_plan("stock", [1], [], 18000, [], [], ["a"])}
            runner._save(sweep, plan)
            original = (runner.client_command, runner.POLL_SECONDS, runner.MARKER)
            runner.client_command = lambda job, plan, job_dir: [sys.executable, str(fake), "--out", str(job_dir)]
            runner.POLL_SECONDS = 0.05
            runner.MARKER = Path(tmp) / "sweep.enabled"
            try:
                args = type("Args", (), {"out": str(sweep), "max_parallel": 0, "keep_raw": False})()
                self.assertEqual(runner.cmd_submit(args), 0)
                self.assertFalse(runner.MARKER.exists())
            finally:
                runner.client_command, runner.POLL_SECONDS, runner.MARKER = original
            plan = runner._load(sweep)
            self.assertEqual(plan["jobs"][0]["state"], "done")
            self.assertEqual(plan["jobs"][0]["summary"]["runs"], 2)
            self.assertEqual(plan["jobs"][0]["summary"]["outcomes"], {"galactic_ai_quality_stock_reference_s1_t3000": "passed"})
            self.assertEqual(sorted(p.name for p in (sweep / "logs").iterdir()),
                             ["stock-async-s001-t3000.ndjson.gz", "stock-reference-s001-t3000.ndjson.gz"])
            self.assertEqual(list((sweep / "jobs" / "j01").glob("*.test.log")), [])
            index = json.loads((sweep / "index.json").read_text())
            self.assertEqual({e["arm"] for e in index}, {"reference", "async"})
            self.assertTrue(all(e["complete"] for e in index))
            self.assertEqual(len(logs.load_directory(sweep / "logs")), 2)

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_registration_matches_the_runner(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / "t.cmake"
            script.write_text(
                "function(eawr_add_game_data_test)\n cmake_parse_arguments(PARSE_ARGV 0 arg \"\" \"NAME\" \"COMMAND\")\n"
                " message(STATUS \"CASE ${arg_NAME} ${arg_COMMAND}\")\nendfunction()\nfunction(set_tests_properties)\nendfunction()\n"
                f'include("{(ROOT / "cmake/EawrGcQuality.cmake").as_posix()}")\n'
                f'eawr_register_gc_quality_cases("{runner.MATRIX.as_posix()}" prog)\n', encoding="utf-8")
            done = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, timeout=60)
            self.assertEqual(done.returncode, 0, done.stderr)
            registered = [line.split()[2] for line in (done.stdout + done.stderr).splitlines() if "CASE " in line]
            # CMake lists object members alphabetically, the runner keeps file order: compare as sets.
            self.assertEqual(sorted(registered), sorted(c["name"] for c in runner.registered_cases()))
            self.assertEqual(len(set(registered)), len(registered))


class CorpusTests(unittest.TestCase):
    def test_compare_trees_lists_exactly_what_moved(self):
        with tempfile.TemporaryDirectory() as tmp:
            staged, fixtures = Path(tmp) / "staged", Path(tmp) / "fixtures"
            for base in (staged / "corpus", fixtures / "corpus"):
                base.mkdir(parents=True)
            (fixtures / "corpus/README.md").write_text("static", encoding="utf-8")
            for folder, replay, csv in ((fixtures, b"AAAA", "t,h\n1,a\n2,b\n"), (staged, b"AAAB", "t,h\n1,a\n2,c\n")):
                (folder / "corpus/a.eawr-gcreplay").write_bytes(replay)
                (folder / "corpus/a.hashes.csv").write_text(csv, encoding="utf-8")
            (staged / "corpus/same.eawr-gcreplay").write_bytes(b"S")
            (fixtures / "corpus/same.eawr-gcreplay").write_bytes(b"S")
            (staged / "corpus/new.eawr-gcreplay").write_bytes(b"N")
            deltas = {d.path: d for d in corpus_upgrade.compare_trees(staged, fixtures)}
            self.assertEqual(deltas["corpus/a.eawr-gcreplay"].status, "moved")
            self.assertIn("offset 3", deltas["corpus/a.eawr-gcreplay"].detail)
            self.assertEqual(deltas["corpus/a.hashes.csv"].detail, "1 differing rows, first at line 3")
            self.assertEqual(deltas["corpus/same.eawr-gcreplay"].status, "unchanged")
            self.assertEqual(deltas["corpus/new.eawr-gcreplay"].status, "new")
            self.assertEqual(deltas["corpus/README.md"].status, "not_regenerated")

    def test_pins_of_the_checked_in_corpus_match_their_files(self):
        deltas = corpus_upgrade.compare_trees(ROOT / corpus_upgrade.FIXTURES, ROOT / corpus_upgrade.FIXTURES)
        pins = corpus_upgrade.compare_pins(ROOT, deltas)
        self.assertEqual(len(pins), 12)
        self.assertTrue(all(p["pin_matches_file"] for p in pins), [p["path"] for p in pins if not p["pin_matches_file"]])
        self.assertFalse(any(d.status != "unchanged" for d in deltas if d.status != "not_regenerated"))


class TechupReport(unittest.TestCase):
    def run_with(self, changes, ticks=27000):
        meta = {"r": "meta", "schema": logs.SCHEMA, "seed": 1, "arm": "reference", "profile": "raw", "campaign": "c", "ticks": ticks, "human": 1,
                "ai": [{"p": 2, "type": "BasicEmpire", "faction": "Empire"}]}
        rows = [meta] + [{"r": "tech", "t": t, "p": 2, "tech": level, "mx": 5} for t, level in changes]
        rows.append({"r": "end", "ticks": ticks, "hashes": ticks, "hash_fnv": 1, "ai_orders": 0, "ai_rejected": 0, "wall_s": 1.0})
        return logs.parse_rows(rows)[0]

    def test_cap_reached_inside_the_window_is_teched_up(self):
        run = self.run_with([(1, 2), (5000, 3), (12000, 4), (18000, 5)])
        row = techup.tech_rows(run, 27000)[0]
        self.assertTrue(row["teched_up"])
        self.assertEqual(row["cap_tick"], 18000)
        self.assertEqual(row["marks"][9000], 3)
        self.assertEqual(row["first"], {3: 5000, 4: 12000, 5: 18000})

    def test_flat_row_and_late_cap_are_not_teched_up(self):
        flat = techup.tech_rows(self.run_with([(1, 1)]), 27000)[0]
        late = techup.tech_rows(self.run_with([(1, 1), (7200, 2), (30000, 5)]), 27000)[0]
        self.assertFalse(flat["teched_up"])
        self.assertFalse(late["teched_up"])
        self.assertIn("NO", techup.render([self.run_with([(1, 1)])], 27000))

    def test_goal_lines_are_summarised(self):
        run = self.run_with([(1, 1)])
        run.tgoals.append({"r": "tgoal", "t": 600, "line": "player 2 goal=7 definition=ADVANCE_TECH_EMPIRE target=0 desire=1 active=1 plan=EMPIREADVANCETECHPLAN running=99 retained_move_requests=0"})
        entry = techup.goal_rows(run)[2]["details"]["ADVANCE_TECH_EMPIRE"]
        self.assertEqual((entry["samples"], entry["running"], entry["first_running"]), (1, 1, 600))


if __name__ == "__main__":
    unittest.main()
