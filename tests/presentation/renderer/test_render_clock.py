"""FD-74: committed admission drives the shared tactical render clock."""

import os
import pathlib
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "tools"))
from native_cpp_fixture import compile_fixture, select_toolchain
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from viewer_mode_sources import source_text  # noqa: E402


class RenderClockContract(unittest.TestCase):
    def test_pause_admission_speed_and_skipped_display_frames(self):
        toolchain = select_toolchain()
        self.assertIsNotNone(toolchain, "the CPU contract requires a native C++ toolchain")
        # Compile the production clock, time controls and TIME conversion. The
        # fixture supplies only the admitted-frame producer, not a shader clock.
        space_header = (ROOT / "include/eawr/presentation/space/space.hpp").read_text()
        constants = "\n".join(re.findall(r"inline constexpr (?:double|std::uint64_t) effect_clock_.*?;", space_header))
        conversion = (ROOT / "src/presentation/space/space_effects.cpp").read_text()
        conversion = conversion.split("float environment_effect_time(", 1)[1].split("\n}", 1)[0]
        conversion = "float environment_effect_time(" + conversion + "\n}"
        source = r'''
#include "__CLOCK__"
#include "__CONTROLS__"
#include <cassert>
#include <cmath>
#include <iostream>
__CONSTANTS__
__CONVERSION__
using eawr::presentation::RenderClock;
using eawr::presentation::ui::TimeControls;
double effect_time(const RenderClock& clock) { return environment_effect_time(clock.sample()); }
int main() {
    RenderClock clock;
    TimeControls time;
    std::uint64_t admitted = 0;
    for (int frame = 0; frame < 30; ++frame) {
        clock.observe(++admitted);
        assert(std::abs(effect_time(clock) - admitted * 0.03) < 0.00001);
    }
    assert(time.press_pause(admitted));
    const auto paused = effect_time(clock);
    for (int redraw = 0; redraw < 240; ++redraw) {
        if (time.running()) ++admitted;
        clock.observe(admitted);
        assert(effect_time(clock) == paused);
    }
    assert(time.resume(admitted));
    // TM-02's whole-millisecond pacing: targets 30/60 admit 30/62
    // frames in this one-second producer. TIME scales with those actual
    // counts rather than an independently accumulated wall-time delta.
    RenderClock normal, fast;
    const auto normal_count = 1000U / (1000U / time.target_rate());
    time.set_speed_step(4, admitted);
    const auto fast_count = 1000U / (1000U / time.target_rate());
    normal.observe(normal_count);
    fast.observe(fast_count);
    const double ratio = effect_time(fast) / effect_time(normal);
    assert(ratio > 1.9 && ratio < 2.2);
    // Slow display: a batch of admissions and per-frame observations agree.
    RenderClock batched;
    batched.observe(admitted);
    clock.observe(admitted + 17);
    batched.observe(admitted + 17);
    assert(effect_time(clock) == effect_time(batched));
    batched.observe(admitted); // An older snapshot cannot rewind the clock.
    assert(batched.admitted_frames() == admitted + 17);
    assert(environment_effect_time(effect_clock_wrap_ticks) == 0.0F);
    std::cout << "paused=" << paused << " normal=" << effect_time(normal)
              << " fast=" << effect_time(fast) << " ratio=" << ratio << '\n';
}
'''
        replacements = {
            "__CLOCK__": (ROOT / "include/eawr/presentation/render_clock.hpp").as_posix(),
            "__CONTROLS__": (ROOT / "src/presentation/ui/time_controls.cpp").as_posix(),
            "__CONSTANTS__": constants, "__CONVERSION__": conversion,
        }
        for old, new in replacements.items():
            source = source.replace(old, new)
        variable = "INCLUDE" if toolchain.msvc else "CPLUS_INCLUDE_PATH"
        toolchain.environment[variable] = str(ROOT / "include") + os.pathsep + toolchain.environment.get(variable, "")
        with tempfile.TemporaryDirectory(prefix="eawr-render-clock-cpu-") as temporary:
            directory = pathlib.Path(temporary)
            cpp = directory / "clock.cpp"
            executable = directory / ("clock.exe" if os.name == "nt" else "clock")
            cpp.write_text(source, encoding="utf-8")
            compile_fixture(toolchain, cpp, executable)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            print(result.stdout.strip())

    def test_every_fixed_tactical_consumer_reads_the_same_admission(self):
        frame = source_text("apps/viewer/src/live_session_frame.cpp")
        observe = frame.index("render_clock_.observe(latest->completed_tick())")
        sample = frame.index("population.defer_unit_sample(render_clock_.sample())")
        pose = frame.index("population.pose_pending_units(renderer)")
        self.assertLess(observe, sample)
        self.assertLess(sample, pose)
        environment = (ROOT / "apps/viewer/src/space_environment_view.cpp").read_text()
        live = environment.index("populate_result_->live(*renderer_, delta, camera_)")
        sample = environment.index("tick = populate_result_->render_clock->sample()")
        effects = environment.index("advance_attached()", sample)
        self.assertLess(live, sample)
        self.assertLess(sample, effects)
        self.assertLess(effects, environment.index("advance_effects(tick)", effects))
        binding = (ROOT / "apps/viewer/src/map_mode_ready_population.cpp").read_text()
        self.assertIn('if (!map_state->options.attract) result.render_clock = &live_session->render_clock()', binding)
        self.assertIn('if (options_.fixture != "attract") population.defer_unit_sample', frame)


if __name__ == "__main__":
    unittest.main()
