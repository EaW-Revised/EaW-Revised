"""--eawr-perf-no-render-timing: the perf trace without the per-frame render-time queries.

With the Separate render thread, viewport_get_measured_render_time_cpu/gpu make the engine synchronize with the render
thread on every frame, so a perf trace distorts the very frames it records. The opt-in flag skips both queries and never asks the
engine to measure the root viewport; the two columns stay in the header and are empty ("not measured"). Without the flag the
trace is unchanged.

The structural half runs everywhere. The graphical half is opt-in on the viewer GPU lane: set
EAWR_GODOT_VIEWER_RUNTIME_TEST, EAWR_GODOT_EXECUTABLE and EAWR_EAW_GAME_ROOT.
"""

import csv
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "apps/viewer/src"
RUNTIME = bool(os.environ.get("EAWR_GODOT_VIEWER_RUNTIME_TEST") and os.environ.get("EAWR_GODOT_EXECUTABLE")
               and os.environ.get("EAWR_EAW_GAME_ROOT"))
FLAG = "--eawr-perf-no-render-timing"


class PerfTraceNoRenderTimingStructure(unittest.TestCase):
    def test_the_flag_gates_both_queries_and_the_measurement_request(self):
        source = (SRC / "perf_trace.cpp").read_text(encoding="utf-8")
        self.assertIn(f'String("{FLAG}")', source)
        # Default on, reset by every parse.
        self.assertIn("render_timing_{true}", source)
        self.assertIn("render_timing_=true;", source)
        self.assertIn("if (render_timing_) RenderingServer::get_singleton()->viewport_set_measure_render_time", source)
        self.assertIn("render_timing_ && viewport_.is_valid()", source)
        # The only callers of the queries are guarded by that value.
        self.assertEqual(source.count("viewport_get_measured_render_time_cpu"), 1)
        self.assertEqual(source.count("viewport_get_measured_render_time_gpu"), 1)
        self.assertEqual(source.count("viewport_set_measure_render_time"), 1)

    def test_no_other_viewer_source_queries_the_render_time_of_the_trace(self):
        for path in SRC.glob("*.cpp"):
            if path.name == "perf_trace.cpp":
                continue
            text = path.read_text(encoding="utf-8")
            self.assertNotIn("viewport_get_measured_render_time", text, path.name)

    def test_the_flag_is_documented(self):
        for relative in ("apps/viewer/README.md", "docs/performance/battle-bench.md"):
            self.assertIn(FLAG, (ROOT / relative).read_text(encoding="utf-8"), relative)


@unittest.skipUnless(RUNTIME, "requires the viewer GPU lane, the engine and game data")
class PerfTraceNoRenderTimingGpu(unittest.TestCase):
    def trace(self, directory, name, extra, engine=()):
        path = Path(directory) / f"{name}.csv"
        command = [os.environ["EAWR_GODOT_EXECUTABLE"], "--resolution", "1280x720", *engine,
                   "--path", str(ROOT / "apps/viewer/project"), "--",
                   "--eawr-map", "data/art/maps/_mp_space_coruscant.ted", "--eawr-populate",
                   "--eawr-live-session", "skirmish", "--eawr-live-ticks", "8", "--eawr-live-workers", "4",
                   "--eawr-game-root", os.environ["EAWR_EAW_GAME_ROOT"], "--eawr-perf-trace", str(path), *extra]
        completed = subprocess.run(command, cwd=ROOT, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   timeout=360, check=False)
        self.assertEqual(completed.returncode, 0, completed.stdout[-4000:])
        with path.open(newline="", encoding="utf-8") as stream:
            return list(csv.reader(stream)), completed.stdout

    def test_the_trace_keeps_its_columns_and_leaves_the_two_render_columns_empty(self):
        with tempfile.TemporaryDirectory(prefix="eawr-perf-timing-") as directory:
            timed, _ = self.trace(directory, "timed", [])
            untimed, log = self.trace(directory, "untimed", [FLAG], ("--render-thread", "separate"))
        self.assertEqual(timed[0], untimed[0], "the header must not change")
        cpu, gpu = timed[0].index("render_cpu_ms"), timed[0].index("render_gpu_ms")
        self.assertGreater(len(timed), 1)
        self.assertGreater(len(untimed), 1)
        for row in timed[1:]:
            float(row[cpu]), float(row[gpu])
        for row in untimed[1:]:
            self.assertEqual((row[cpu], row[gpu]), ("", ""))
            self.assertEqual(len(row), len(untimed[0]))
        # The engine warned about the synchronization before; with the flag it must not.
        self.assertNotIn("viewport_get_measured_render_time", log)


if __name__ == "__main__":
    unittest.main()
