"""Exercise the viewer's production load scheduler without an engine or game data."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain


class LoadJobContracts(unittest.TestCase):
    def test_order_cancellation_failure_and_heap_ownership(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest("requires a native C++20 fixture compiler")
        fixture = (ROOT / "tests/presentation/renderer/load_job_fixture.cpp.in").read_text(encoding="utf-8")
        runner = (ROOT / "apps/viewer/src/attract_runner.cpp").read_text(encoding="utf-8")
        start = runner.index("void free_battle() {") + len("void free_battle() {")
        depth, end = 1, start
        while depth:
            if runner[end] == "{":
                depth += 1
            elif runner[end] == "}":
                depth -= 1
            end += 1
        fixture = fixture.replace("__FREE_BATTLE__", runner[start:end - 1])
        header = (ROOT / "apps/viewer/src/load_job.hpp").as_posix()
        with tempfile.TemporaryDirectory(prefix="eawr-load-job-") as temporary:
            directory = Path(temporary)
            cpp, executable = directory / "load_job.cpp", directory / "load_job.exe"
            cpp.write_text(fixture.replace('"load_job.hpp"', f'"{header}"'), encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("heap ownership PASS", ran.stdout)

    def test_offloaded_stages_run_on_one_worker_and_cancel(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest("requires a native C++20 fixture compiler")
        fixture = (ROOT / "tests/presentation/renderer/offloaded_stages_fixture.cpp.in").read_text(encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="eawr-offloaded-stages-") as temporary:
            directory = Path(temporary)
            cpp, executable = directory / "offloaded_stages.cpp", directory / "offloaded_stages.exe"
            cpp.write_text(fixture.replace('"load_job.hpp"', f'"{(ROOT / "apps/viewer/src/load_job.hpp").as_posix()}"')
                           .replace('"offloaded_stages.hpp"', f'"{(ROOT / "apps/viewer/src/offloaded_stages.hpp").as_posix()}"'),
                           encoding="utf-8")
            compile_fixture(toolchain, cpp, executable, (str(ROOT / "include"),))
            ran = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertIn("OffloadedStages order, failure, cancel, recording and poll PASS", ran.stdout)

    def test_attract_offload_is_gated_and_stages_stay_off_the_engine(self):
        # The worker runs stages 1-3 only; they must name no engine object (checklist 1) and no view options.
        local = (ROOT / "apps/viewer/src/battle_source_local.cpp").read_text(encoding="utf-8")
        stages_1_to_3 = local.split("stages.push_back", 1)[1].split("// 4: the AI scripts", 1)[0]
        for forbidden in ("godot::", "UtilityFunctions", "RenderingServer", "ResourceLoader", "MenuTimeline",
                          "options_.attract_ai", "options_.skirmish.map", "draw_bindings_"):
            self.assertNotIn(forbidden, stages_1_to_3, forbidden)
        stages = (ROOT / "apps/viewer/src/offloaded_stages.hpp").read_text(encoding="utf-8")
        for forbidden in ("godot::", "godot_cpp", "MenuTimeline"):
            self.assertNotIn(forbidden, stages)
        prepare = (ROOT / "apps/viewer/src/live_session_prepare.cpp").read_text(encoding="utf-8")
        self.assertIn("options_.load_offload ?", prepare)
        self.assertIn("poll(std::chrono::microseconds(0))", prepare)
        self.assertIn("yield_load_frame();", prepare)
        self.assertIn("OffloadedStages::run_guarded(stage, run->failure)", prepare)
        ready = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        self.assertIn("load_offload = separate", ready)

    def test_mode_cancels_before_renderer_teardown_and_gameplay_stays_synchronous(self):
        assets = (ROOT / "apps/viewer/src/map_mode_assets.cpp").read_text(encoding="utf-8")
        teardown = assets.split("MapMode::State::~State() {", 1)[1].split("\n    }", 1)[0]
        self.assertLess(teardown.index("ready_load.reset();"), teardown.index("space.reset();"))
        source = (ROOT / "apps/viewer/src/map_mode_ready.cpp").read_text(encoding="utf-8")
        ready = source.split("bool MapMode::ready(", 1)[1].split("void MapMode::begin_ready", 1)[0]
        self.assertIn("job.run_to_completion()", ready)
        self.assertNotIn("run_for(", ready)
        advance = source.split("MapMode::ReadyStatus MapMode::advance_ready(const bool input_pending)", 1)[1]
        self.assertIn("job.run_for(std::chrono::milliseconds(8), input_pending)", advance)


if __name__ == "__main__":
    unittest.main()
