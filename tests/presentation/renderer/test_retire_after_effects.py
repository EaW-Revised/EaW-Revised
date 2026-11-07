"""L1.1: the battle effects read a ship still owed its retirement exactly as they read a retired one."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    return source[start:source.index('\n}', source.index('{', start)) + 2]


HARNESS = r"""
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>
namespace sim { using EntityId = std::uint64_t;
namespace math { struct Mat3x4 { int value{}; };
template<class T> struct Res { std::optional<T> v; explicit operator bool() const { return v.has_value(); } const T& value() const { return *v; } };
inline Res<Mat3x4> compose(const Mat3x4& a, const Mat3x4& b) { return {Mat3x4{a.value + b.value}}; } } }
namespace scene { inline float fixed_from_binary32(float v) { return v; } }
namespace particles { inline std::optional<sim::math::Mat3x4> fixed_model_frame(int model, float (*)(float)) { return sim::math::Mat3x4{model}; } }
struct SpacePopulation {
    struct LiveBoneFrame final { sim::math::Mat3x4 transform; bool visible{}; };
    struct Posed { int model_asset{}; bool visible = true; };
    struct Placed { sim::EntityId live_entity{}; };
    struct Options { std::vector<Placed> placed_ships; } options_;
    std::map<sim::EntityId, std::size_t> contact_live_ships_;
    std::vector<bool> live_retired_;
    std::vector<std::optional<sim::math::Mat3x4>> live_transforms_;
    std::vector<std::vector<Posed>> contact_bones_;
    bool live_ship_drawn(std::size_t) const noexcept { return true; }
    std::optional<LiveBoneFrame> live_bone_frame(sim::EntityId entity, std::uint32_t bone,
        std::span<const std::size_t> retiring = {}) const;
    // What retire_live_ship does to the state live_bone_frame reads.
    void retire(std::size_t ship) {
        live_retired_[ship] = true; contact_bones_[ship].clear(); live_transforms_[ship].reset();
        std::erase_if(contact_live_ships_, [&](const auto& entry) { return entry.second == ship; });
    }
};
__FUNCTIONS__
int main() {
    SpacePopulation population;
    for (std::size_t ship = 0; ship < 4; ++ship) {
        population.options_.placed_ships.push_back({100 + ship});
        population.live_retired_.push_back(false);
        population.live_transforms_.push_back(sim::math::Mat3x4{static_cast<int>(ship)});
        population.contact_bones_.push_back({{7, true}, {8, false}});
        population.contact_live_ships_[100 + ship] = ship;
    }
    const std::vector<std::size_t> retiring{1, 3};
    const auto snapshot = [&](std::span<const std::size_t> owed) {
        std::vector<std::optional<SpacePopulation::LiveBoneFrame>> rows;
        for (sim::EntityId entity = 99; entity < 106; ++entity)
            for (std::uint32_t bone = 0; bone < 3; ++bone) rows.push_back(population.live_bone_frame(entity, bone, owed));
        return rows;
    };
    const auto none = snapshot({});
    assert(none[(101 - 99) * 3].has_value() && none[(103 - 99) * 3 + 1].has_value());
    // Retire-mid-contact twin: "owed" before the retirement equals "retired" after it, row for row. The retired side is
    // modeled (retire() below mirrors what retire_live_ship does to the state live_bone_frame reads), not the production function.
    const auto owed = snapshot(retiring);
    for (const std::size_t ship : retiring) population.retire(ship);
    const auto retired = snapshot({});
    assert(owed.size() == retired.size());
    for (std::size_t row = 0; row < owed.size(); ++row) {
        assert(owed[row].has_value() == retired[row].has_value());
        if (owed[row]) assert(owed[row]->transform.value == retired[row]->transform.value && owed[row]->visible == retired[row]->visible);
    }
    assert(!owed[(101 - 99) * 3] && !owed[(103 - 99) * 3] && owed[(100 - 99) * 3] && owed[(102 - 99) * 3 + 1]);
    return 0;
}
"""


class RetireAfterEffects(unittest.TestCase):
    def test_owed_ships_read_like_retired_ones(self):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        body = function('apps/viewer/src/space_populate_animation.cpp',
                        'std::optional<SpacePopulation::LiveBoneFrame> SpacePopulation::live_bone_frame(')
        with tempfile.TemporaryDirectory(prefix='eawr-retire-after-effects-') as temp:
            directory = Path(temp)
            cpp = directory / 'retire.cpp'
            exe = directory / ('retire.exe' if os.name == 'nt' else 'retire')
            cpp.write_text(HARNESS.replace('__FUNCTIONS__', body))
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_hook_order(self):
        source = (ROOT / 'apps/viewer/src/map_mode_ready_population.cpp').read_text()
        finish = source[source.index('auto finish = ['):]
        effects_at = finish.index('effects_frame()')
        retire_at = finish.index('retire_clones();', effects_at)
        self.assertLess(retire_at, finish.index('debris->effects('))
        self.assertLess(finish.index('debris->effects('), finish.rindex('finish_flush.flush();'))
        lane = source[source.index('if (frame_input->lane) {'):source.index('if (!on_lane) {')]
        self.assertIn('if (!effects_frame())', lane)
        finish_lane = source[source.index('auto finish_lane = ['):source.index('auto finish = [')]
        self.assertLess(finish_lane.index('settle_lane();'), finish_lane.index('retire_clones();'))
        # The gesture feedback is taken once, in the pre phase, before the window runs.
        self.assertEqual(len(re.findall(r'take_ability_clicks\(\)', source)), 2)  # decltype + the take
        self.assertLess(source.index('ability_clicks = live_session->take_ability_clicks()'),
                        source.index('const bool combat_owed = window(true)'))


if __name__ == '__main__':
    unittest.main()
