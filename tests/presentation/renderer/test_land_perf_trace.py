"""#6475: a land run with --eawr-perf-trace writes the same per-frame rows as space.

The land view used to write a hand-built row with a handful of columns (no sim tick phases, no submit cost). Space and
land now share one writer, MapMode::State::record_frame_trace, and the land-only inputs (the submit cost of the land
snapshot) are measured only while the trace is on. This structural contract runs everywhere and keeps the writer land-safe:
no dereference of a space-only object, no second hand-built row, no clock reads without the trace. The row content itself is
asserted on the rig by test_land_live.LandLiveGpu.test_human_squad_click_move_stop_on_terrain.
"""

from pathlib import Path
import re
import unittest

SRC = Path(__file__).resolve().parents[3] / "apps/viewer/src"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    depth, index = 0, source.index("{", start)
    begin = index
    while True:
        depth += {"{": 1, "}": -1}.get(source[index], 0)
        if depth == 0:
            return source[begin:index + 1]
        index += 1


class LandPerfTraceStructure(unittest.TestCase):
    def setUp(self):
        self.live = (SRC / "map_mode_live.cpp").read_text(encoding="utf-8")
        self.land = (SRC / "map_mode_land_live.cpp").read_text(encoding="utf-8")

    def test_the_land_frame_uses_the_shared_writer_only(self):
        body = function_body(self.land, "MapMode::State::process_land_live(")
        self.assertIn("record_frame_trace(delta, ui_recording);", body)
        self.assertNotIn("PerfTrace::Frame", body, "land must not build its own row again")
        self.assertNotIn("perf_trace->frame(", body)
        self.assertIn("process_land_live(delta, ui_recording)", self.live)

    def test_the_shared_writer_never_dereferences_space_only_objects_unguarded(self):
        lines = function_body(self.live, "MapMode::State::record_frame_trace(").splitlines()
        body = "\n".join(line for line in lines if not line.lstrip().startswith("//"))
        for match in re.finditer(r"state\.space->", body):
            before = body[:match.start()]
            guards = ("state.space ? state.space->", "if (state.space) {", "state.space && state.space->")
            line_start = before.rfind("\n") + 1
            context = body[line_start:match.end()]
            window = before[-400:] + context
            self.assertTrue(any(guard in window for guard in guards), f"unguarded space dereference: {context.strip()}")
        self.assertNotIn("state.space_population->", body, "use the shared population (space or land scene)")
        self.assertIn("state.land_population.get()", body)

    def test_land_submit_cost_is_measured_only_with_the_trace(self):
        draw = function_body(self.live, "MapMode::State::draw_map_frame(")
        self.assertIn("if (state.perf_trace && state.land_population) {", draw)
        self.assertIn("land_submit_ms", draw)
        land = function_body(self.land, "MapMode::State::process_land_live(")
        self.assertIn("FrameTimer copy_timer(perf_trace ? &land_submit_ms : nullptr, true);", land)
        body = function_body(self.live, "MapMode::State::record_frame_trace(")
        self.assertIn("state.land_submit_ms", body)
        self.assertIn("state.land_submit_pieces", body)
        self.assertIn("state.land_submit_sent", body)

    def test_the_land_session_and_scene_record_frame_costs_under_the_trace(self):
        self.assertIn("live_session->trace_frames(perf_trace.has_value());", self.land)
        self.assertIn("land_population->trace_frames(perf_trace.has_value());", self.land)


if __name__ == "__main__":
    unittest.main()
