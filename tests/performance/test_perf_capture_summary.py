"""perf_capture_summary.py: feed, capture and GC-trace inputs, the hitch rule, the pattern readout and the verdict rules."""
import importlib
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools/performance"))
summary = importlib.import_module("perf_capture_summary")

INTERVAL = 33.3


def feed_lines(ticks=600, spike_every=300, frames=False, marker=None, lock=0.0, cpu=0.9):
    """A tactical feed at 1x: 30 ticks/s, `step` 8 ms, a 'command-source' spike every `spike_every` ticks."""
    rows = [dict(r="header", schema=1, build="test", engine="e", t=0, clock="steady-us"),
            dict(r="source", src=0, label="tactical", t=0),
            {"r": "phase-name", "src": 0, "id": 0, "name": "step"},
            {"r": "phase-name", "src": 0, "id": 1, "name": "command-source"}]
    for n in range(1, ticks + 1):
        spike = spike_every and n % spike_every == 0
        source_ms = 40.0 if spike else 1.0
        rows.append(dict(r="tick", src=0, n=n, t=int(n * INTERVAL * 1000), ms=8.0 + source_ms, lag=0.0, lock=lock, cpu=cpu,
                         iv=INTERVAL, fl=0, ph=[[0, 8.0], [1, source_ms]]))
        if spike and marker:
            rows.append(dict(r="marker", src=0, k=marker, n=n, v=0, fs=0))
        if frames:
            rows.append(dict(r="frame", n=n, t=int(n * INTERVAL * 1000), ms=16.0, main=5.0, render=6.0, gpu=14.0, lock=0.0, stale=0))
    return rows


def write(directory, name, rows):
    path = Path(directory) / name
    path.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    return path


class SummaryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def load(self, rows, skip=0):
        return summary.load([write(self.tmp.name, "feed.jsonl", rows)], skip)

    def test_feed_is_read_with_phase_names(self):
        capture = self.load(feed_lines(ticks=40))
        source = capture.sources[0]
        self.assertEqual(source.label, "tactical")
        self.assertEqual(len(source.ticks), 40)
        self.assertEqual(source.ticks[0].phases, {"step": 8.0, "command-source": 1.0})
        self.assertEqual(capture.header["build"], "test")

    def test_percentiles_and_hitch_list_with_spacing_and_dominant_phase(self):
        capture = self.load(feed_lines())
        ticks = capture.sources[0].ticks
        self.assertAlmostEqual(summary.percentile([t.ms for t in ticks], 0.5), 9.0)
        hitches = summary.detect_hitches(ticks)
        self.assertEqual([h["tick"] for h in hitches], [300, 600])
        self.assertEqual(hitches[1]["gap_ticks"], 300)
        self.assertAlmostEqual(hitches[1]["gap_s"], 300 * INTERVAL / 1000, places=2)
        self.assertEqual(hitches[0]["phase"], "command-source")  # not "step": the excess decides, not the size
        self.assertAlmostEqual(hitches[0]["excess"], 39.0, places=1)

    def test_pattern_tick_locked_with_cadence_and_marker(self):
        capture = self.load(feed_lines(ticks=3300, spike_every=300, marker="gc.ai.perception"))
        source = capture.sources[0]
        pattern = summary.analyze_pattern(summary.detect_hitches(source.ticks), source.markers)
        self.assertEqual(pattern["kind"], "tick-locked")
        self.assertEqual(pattern["period"], 300)
        self.assertEqual(pattern["cadence"], "gc ai perception/learning (aligned)")
        self.assertEqual(pattern["marker"], "gc.ai.perception")
        line = summary.format_pattern(pattern)
        self.assertIn("every 300 ticks (11/11, tick-locked, phase 0)", line)
        self.assertIn("command-source +39.0 ms", line)
        self.assertIn("marker gc.ai.perception 11/11", line)

    def test_pattern_none_for_irregular_hitches(self):
        hitches = [dict(tick=t, t_us=t * 33300, ms=50.0, median=9.0, phase="step", excess=40.0)
                   for t in (101, 160, 333, 361, 700, 947, 1004, 1555)]
        pattern = summary.analyze_pattern(hitches, [])
        self.assertEqual(pattern["kind"], "none")
        self.assertIn("pattern: none (8 hitches, irregular)", summary.format_pattern(pattern))

    def test_pattern_wall_locked_when_ticks_run_at_another_speed(self):
        # a 10 s wall cause at 4x: the wall spacing is constant, the tick spacing wobbles
        hitches = []
        wall = 0
        for i in range(12):
            wall += 10_000_000
            tick = int(wall / 8325) + (i % 5) * 7
            hitches.append(dict(tick=tick, t_us=wall, ms=40.0, median=5.0, phase="", excess=0.0))
        pattern = summary.analyze_pattern(hitches, [])
        self.assertEqual(pattern["kind"], "wall-locked")
        self.assertAlmostEqual(pattern["period"], 10000, delta=30)

    def test_allocation_driven_readout(self):
        hitches = [dict(tick=t, t_us=t * 33300, ms=50.0, median=9.0, phase="step", excess=40.0)
                   for t in (101, 160, 333, 361, 700, 947, 1004, 1555)]
        markers = [("lua.collect", h["tick"]) for h in hitches]
        pattern = summary.analyze_pattern(hitches, markers)
        self.assertEqual(pattern["kind"], "none")
        self.assertTrue(pattern["allocation_driven"])

    def test_verdict_rules(self):
        base = dict(idle=False, delivered=1.0, interval=33.3, tick_p95=10.0, mean_tick=9.0, lock_ms=0.0, cpu=0.9,
                    top_phase="command-source", top_ms=31.0, budget=16.7, frame_p95=10.0, mean_frame=9.0, ui_lock_ms=0.0,
                    stale=0.0, gpu=None, render=None, main=None, others=None)

        def v(**kw):
            return summary.evaluate_verdict({**base, **kw})

        self.assertEqual(v(idle=True), "idle")                                                       # rule 0
        self.assertEqual(v(), "ok")                                                                  # rule 1
        self.assertEqual(v(delivered=0.8, tick_p95=33.0), "sim-bound: command-source 31.0 ms")       # rule 2
        self.assertTrue(v(delivered=0.8, lock_ms=3.0, mean_tick=9.0).startswith("coupled: sim waits on frame"))  # rule 3
        self.assertEqual(v(delivered=0.8, cpu=0.3, others=0.31), "external: other processes 31 %")   # rule 4
        self.assertEqual(v(delivered=0.8, cpu=0.3), "coupled: sim thread starved of CPU (own threads)")  # rule 5
        self.assertEqual(v(delivered=0.8, cpu=0.7), "sim-bound (unattributed)")                      # rule 6
        self.assertEqual(v(frame_p95=20.0, gpu=18.0), "render-bound: GPU 18.0 ms")                   # rule 7
        self.assertEqual(v(frame_p95=20.0, render=18.0), "render-bound: render thread 18.0 ms")      # rule 8
        self.assertTrue(v(frame_p95=20.0, ui_lock_ms=5.0, mean_frame=20.0).startswith("coupled: frame waits on sim"))  # rule 9
        self.assertTrue(v(frame_p95=20.0, main=12.0).startswith("render-bound: main thread (viewer 12.0 ms"))  # rule 10
        both = v(delivered=0.8, tick_p95=33.0, frame_p95=20.0, gpu=18.0)
        self.assertTrue(both.startswith("sim-bound + render-bound (independent)"))
        self.assertTrue(v(delivered=0.8, tick_p95=33.0, frame_p95=20.0, gpu=18.0, correlation=0.7).startswith("coupled: "))
        self.assertIn("(picture holds)", v(delivered=0.8, tick_p95=33.0, stale=0.4))
        self.assertTrue(v(delivered=0.8, cpu=None).startswith("unknown"))  # no on-CPU reading: no sim cause is claimed

    def test_report_for_a_feed_prints_all_sections(self):
        capture = self.load(feed_lines(ticks=1800, frames=True, marker="gc.ai.perception"))
        text = "\n".join(summary.report(capture, None, False, 16.7))
        for needle in ("== tactical: ticks 1800", "tick ms p50", "tick hitches: 6 of 1800", "hitch interval: ticks p50 300",
                       "pattern: every 300 ticks", "verdict: ", "== frames 1800", "frame hitches: 0"):
            self.assertIn(needle, text)

    def test_slow_feed_reads_sim_bound(self):
        rows = feed_lines(ticks=300, spike_every=0)
        for row in rows:
            if row.get("r") == "tick":
                row["ms"] = 40.0
                row["t"] = int(row["n"] * 50000)  # 20 ticks/s against 30 requested
                row["ph"] = [[0, 8.0], [1, 32.0]]
        capture = self.load(rows)
        stats = summary.window_stats(capture.sources[0], [], [], 16.7)
        self.assertFalse(stats["idle"])
        self.assertAlmostEqual(stats["delivered"], 0.66, delta=0.02)
        self.assertEqual(summary.evaluate_verdict(stats), "sim-bound: command-source 32.0 ms")

    def test_chrome_capture_is_read(self):
        events = [dict(ph="M", pid=1, tid=10000, name="thread_name", args=dict(name="ticks: tactical"))]
        for n in range(1, 41):
            end = n * 33300
            events.append(dict(ph="X", pid=1, tid=10000, name="tick", ts=end - 9000, dur=9000.0, args=dict(tick=n, lag_ms=0.0, cpu=0.8)))
            events.append(dict(ph="X", pid=1, tid=10100, name="step", ts=end - 9000, dur=8000.0, args=dict(tick=n)))
        events.append(dict(ph="i", s="t", pid=1, tid=10090, name="save", ts=33300, args=dict(tick=1, value=0)))
        path = Path(self.tmp.name) / "capture.json"
        path.write_text(json.dumps(dict(traceEvents=events, otherData=dict(schema=1, build="b", engine="e", pattern="x"))),
                        encoding="utf-8")
        capture = summary.load([path], 0)
        source = capture.sources[0]
        self.assertEqual(capture.kind, "chrome")
        self.assertEqual(source.label, "tactical")
        self.assertEqual(len(source.ticks), 40)
        self.assertEqual(source.ticks[3].phases, {"step": 8.0})
        self.assertEqual(source.markers, [("save", 1)])

    def test_gc_trace_still_summarises_with_its_spike_rule(self):
        rows = []
        for n in range(1, 61):
            spike = n % 20 == 0
            rows.append(dict(tick=n, t_ms=n * 33.3, total_ms=60.0 if spike else 5.0, source_ms=50.0 if spike else 1.0, step_ms=5.0,
                             phases={"svc_perception": [1, 50.0 if spike else 0.5, 50.0 if spike else 0.5]}, cpu_summed={}))
        path = write(self.tmp.name, "trace.jsonl", rows)
        capture = summary.load([path], 0)
        self.assertEqual(capture.kind, "gc-trace")
        text = "\n".join(summary.report(capture, None, False, 16.7))
        self.assertIn("ticks >= 33 ms: 3 of 60", text)
        self.assertIn("svc_perception +", text)
        self.assertIn("hitch interval: ticks p50 20", text)
        self.assertIn("svc_perception", text.split("sum excl ms")[1])

    def test_segments_are_joined_and_skip_ticks_applies(self):
        a = write(self.tmp.name, "a.jsonl", feed_lines(ticks=30))
        rows = [r for r in feed_lines(ticks=60) if r.get("n", 999) > 30 or r["r"] != "tick"]
        b = write(self.tmp.name, "b.jsonl", rows)
        capture = summary.load([a, b], 10)
        self.assertEqual(min(t.n for t in capture.sources[0].ticks), 11)
        self.assertEqual(len(capture.sources[0].ticks), 20 + 30)

    def test_context_threads_memory_and_fight_rows_are_read_and_printed(self):
        rows = feed_lines(ticks=60)
        rows.insert(1, dict(r="context", t=1, cpu="Test CPU @ 3.0GHz", cores=12, logical=20, p_cores=8, e_cores=4, p_logical=16,
                            topology=True, gpu="Test GPU", gpu_type="discrete", gpu_driver="Vendor 1.2.3", gpu_api="1.3",
                            renderer="forward_plus", rd_driver="vulkan", res=[2560, 1440], window="fullscreen", vsync="on",
                            fps_cap=120, render_thread="separate", render_thread_setting="separate",
                            video={"detail_level": 2}, threads={"hardware": 20, "sim": {"setting": 0, "automatic": 18,
                            "mode": "automatic", "resolved": 18}, "particles": {"setting": 8, "automatic": 8, "mode": "manual",
                            "resolved": 8}}))
        rows[2:2] = [dict(r="source", src=1, label="galactic", workers=18, t=0)]
        for second in range(1, 11):
            rows.append(dict(r="thr", t=second * 1_000_000, th=[["sim", 0.9, 1], ["main", 0.5 + second / 100, 1], ["t99", 0.1, 0]]))
            rows.append(dict(r="mem", t=second * 1_000_000, commit_mb=1000 + second, ws_mb=800 + second, pf_s=50.0 * second))
        for n in range(1, 41):
            rows.append(dict(r="frame", n=n, t=n * 16000, ms=10.0 + n / 4, main=5.0, render=6.0, gpu=7.0, lock=0.0, stale=0,
                             u=10 * n, eu=5 * n, atk=n, proj=3 * n, fx=n, part=100 * n, dc=500 + n, obj=900 + n))
        capture = self.load(rows)
        self.assertEqual(capture.contexts[0]["cores"], 12)
        self.assertEqual(capture.sources[1].workers, 18)
        self.assertEqual(len(capture.threads), 10)
        self.assertEqual(len(capture.mem), 10)
        self.assertEqual(capture.frames[-1].fight["proj"], 120)
        text = "\n".join(summary.report(capture, None, False, 16.7))
        self.assertIn("8 performance + 4 efficiency cores", text)
        self.assertIn("gpu: Test GPU", text)
        self.assertIn("sim 18 (automatic, automatic would be 18)", text)
        self.assertIn("particles 8 (manual", text)
        self.assertIn("hottest labeled thread: sim", text)
        self.assertIn("commit MiB: first 1001, last 1010, max 1010", text)
        self.assertIn("== fight intensity (40 frames)", text)
        self.assertIn("draw calls", text)
        self.assertIn("Q4", text)
        # Frame time rises with the fight in this series: the correlation is reported as positive.
        self.assertGreater(summary.correlation([float(f.fight["u"]) for f in capture.frames], [f.ms for f in capture.frames]), 0.99)

    def test_main_prints_and_rejects_empty(self):
        path = write(self.tmp.name, "feed.jsonl", feed_lines(ticks=100))
        self.assertEqual(summary.main([str(path)]), 0)
        empty = write(self.tmp.name, "empty.jsonl", [dict(r="header", schema=1)])
        self.assertEqual(summary.main([str(empty)]), 1)


if __name__ == "__main__":
    unittest.main()
