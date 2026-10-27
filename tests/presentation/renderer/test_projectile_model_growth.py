"""Execute the production proxy admission transaction against failing backend stand-ins."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain

POPULATION = r'''
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "eawr/presentation/proxy_reservation.hpp"
using namespace eawr::presentation;
namespace sim { using EntityId = std::uint64_t; using AssetId = std::uint64_t; }
namespace scene {
struct Placement { std::size_t scene_ordinal{}; sim::EntityId entity_id{}; };
struct SpacePlacementDecision { std::size_t scene_ordinal{}; };
struct Scene { std::vector<Placement> placements; };
}
namespace space {
struct PopulationIndex {
    std::vector<std::vector<std::size_t>> rows;
    std::span<const std::size_t> pieces(std::size_t ship) const { return rows.at(ship); }
};
struct AttachmentMarks { std::size_t size{}; template<class T> void configure(const T& states) { size = states.size(); } };
}
struct GodotRenderer {
    using ProxyOutcome = ::ProxyOutcome;
    using ProxyResult = ::ProxyResult;
    struct ProxyInstance { sim::EntityId entity{}, source{}; sim::AssetId asset{}; };
    std::vector<ProxyInstance> live;
    ProxyOutcome outcome{ProxyOutcome::reserved};
    ProxyResult reserve_proxy_instances(std::span<const ProxyInstance> entries) {
        if (outcome != ProxyOutcome::reserved) return {outcome, entries.size() * 128, "injected"};
        live.insert(live.end(), entries.begin(), entries.end());
        return {outcome, entries.size() * 128, {}};
    }
    void forget_instance_pose(sim::EntityId entity) {
        std::erase_if(live, [&](const auto& entry) { return entry.entity == entity; });
    }
};
struct SpacePopulation {
    struct Ship { std::string object_id; bool projectile_slot{}; sim::EntityId live_entity{}; };
    struct Options { using PlacedShip = Ship; std::vector<Ship> placed_ships; } options_;
    struct Piece { struct { sim::EntityId entity_id{}; sim::AssetId asset_id{}; } instance;
        std::optional<std::size_t> live; struct Gate { std::size_t decision{}; }; std::optional<Gate> gate; };
    using GatedInstance = Piece;
    struct LiveClipInstance { std::size_t ship{}; sim::EntityId entity{}; sim::AssetId asset{}; };
    struct AnimatedInstance { sim::EntityId entity{}; sim::AssetId asset{}; std::size_t idle{}; };
    std::vector<Piece> pieces_;
    std::vector<LiveClipInstance> live_clip_instances_;
    std::vector<AnimatedInstance> animated_instances_;
    std::vector<scene::SpacePlacementDecision> decisions_;
    std::vector<std::vector<int>> hardpoint_states_;
    std::vector<std::vector<sim::AssetId>> hull_surface_assets_;
    std::vector<std::optional<std::size_t>> live_decisions_, live_models_, live_clips_, live_alternate_clips_, live_bounds_, live_alternate_counts_;
    std::optional<scene::Scene> scene_{scene::Scene{}};
    space::PopulationIndex piece_index_;
    space::AttachmentMarks attachment_marks_;
    sim::EntityId next_arrival_piece_{500};
    bool composed_{true};
    int scheduled{};
    void schedule_arrival_sample() { ++scheduled; }
    void prepare_compose_slots(bool) {
        const auto size = options_.placed_ships.size();
        live_decisions_.resize(size); hull_surface_assets_.resize(size); live_models_.resize(size);
        live_clips_.resize(size); live_alternate_clips_.resize(size); live_bounds_.resize(size); live_alternate_counts_.resize(size);
    }
    void rebuild_piece_index() {
        piece_index_.rows.assign(options_.placed_ships.size(), {});
        for (std::size_t index = 0; index < pieces_.size(); ++index) piece_index_.rows[*pieces_[index].live].push_back(index);
    }
    ProxyResult append_model_proxies(GodotRenderer&, std::size_t, std::span<const Ship>);
};
__FUNCTIONS__
int main() {
    GodotRenderer renderer;
    SpacePopulation population;
    population.options_.placed_ships.push_back({"authored_missile", true, 100});
    population.prepare_compose_slots(true);
    population.live_decisions_[0] = 0;
    population.hull_surface_assets_[0] = {77};
    population.scene_->placements.push_back({0,1});
    population.decisions_.push_back({0});
    population.hardpoint_states_.push_back({1,2});
    population.pieces_.push_back({{10,77},0,{}});
    population.live_clip_instances_.push_back({0,10,77});
    population.animated_instances_.push_back({10,77,9});
    population.rebuild_piece_index();
    std::vector<SpacePopulation::Ship> ships(32, {"authored_missile", true, 101});
    for (const auto failure : {ProxyOutcome::refused_budget, ProxyOutcome::allocation_failed}) {
        renderer.outcome = failure;
        auto result = population.append_model_proxies(renderer, 0, ships);
        assert(result.outcome == failure && result.bytes == 32 * 128);
        assert(population.options_.placed_ships.size() == 1 && population.pieces_.size() == 1);
        assert(population.live_decisions_.size() == 1 && population.scene_->placements.size() == 1);
        assert(population.piece_index_.rows.size() == 1 && population.hardpoint_states_.size() == 1);
        assert(population.next_arrival_piece_ == 500 && population.scheduled == 0 && renderer.live.empty());
        assert(population.pieces_[0].instance.entity_id == 10 && population.hull_surface_assets_[0][0] == 77);
    }
    renderer.outcome = ProxyOutcome::reserved;
    auto result = population.append_model_proxies(renderer, 0, ships);
    assert(result.outcome == ProxyOutcome::reserved && result.first_slot == 1);
    assert(population.options_.placed_ships.size() == 33 && population.pieces_.size() == 33);
    assert(renderer.live.size() == 32 && population.next_arrival_piece_ == 532 && population.scheduled == 1);
    for (std::size_t index = 1; index < 33; ++index) {
        assert(population.pieces_[index].instance.asset_id == 77);
        assert(population.pieces_[index].instance.entity_id == 499 + index && population.pieces_[index].live == index);
        assert(population.live_clip_instances_[index].ship == index);
        assert(population.animated_instances_[index].idle == 9);
        assert(population.hull_surface_assets_[index][0] == 77);
        assert(renderer.live[index - 1].source == 10);
    }
    assert(population.attachment_marks_.size == 33);
    std::vector<SpacePopulation::Ship> wrong{{"different_model", true, 900}};
    result = population.append_model_proxies(renderer, 0, wrong);
    assert(result.outcome == ProxyOutcome::invalid_recipe && population.pieces_.size() == 33);
}
'''

ELIGIBILITY = r'''
#include <cassert>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
enum class Render { model, laser };
struct ProjectileLook { Render render{Render::model}; std::string projectile{"authored"}; };
namespace tactical { struct Projectile { std::uint64_t id{}, muzzle_delay_until{}; const ProjectileLook* look{}; }; }
struct Snapshot {
    std::vector<tactical::Projectile> shots;
    const auto& projectiles() const { return shots; }
    std::uint64_t completed_tick() const { return 100; }
};
struct Visibility { struct { bool visible{}; bool drawn() const { return visible; } } hide; };
std::map<std::uint64_t, Visibility> projectile_visibility_;
const ProjectileLook* look_of(const tactical::Projectile& shot) { return shot.look; }
auto collect(const Snapshot& latest) {
__LOOP__
return flying;
}
int main() {
    ProjectileLook model, laser{Render::laser,"beam"};
    Snapshot latest;
    for (std::uint64_t id = 1; id <= 33; ++id) { latest.shots.push_back({id,0,&model}); projectile_visibility_[id].hide.visible = true; }
    latest.shots.push_back({34,100,&model}); projectile_visibility_[34].hide.visible = true;
    latest.shots.push_back({35,0,&model}); projectile_visibility_[35].hide.visible = false;
    latest.shots.push_back({36,0,&laser}); projectile_visibility_[36].hide.visible = true;
    latest.shots.push_back({37,0,nullptr}); projectile_visibility_[37].hide.visible = true;
    latest.shots.push_back({38,0,&model}); // no observer record
    projectile_visibility_[39].hide.visible = true; // ended flight absent from latest
    auto wanted = collect(latest);
    assert(wanted.size() == 1 && wanted.at("authored").size() == 33);
    latest.shots.clear();
    assert(collect(latest).empty());
}
'''

def function(source, signature):
    start = source.index(signature)
    return source[start:source.index('\n}', source.index('{', start)) + 2]


class ProjectileProxyGrowth(unittest.TestCase):
    def run_fixture(self, harness):
        toolchain = select_toolchain()
        if toolchain is None:
            if os.name == 'nt':
                self.fail('Windows contract requires the native MSVC fixture toolchain')
            self.skipTest('requires C++ compiler')
        harness = harness.replace('"eawr/', '"' + (ROOT / 'include/eawr').as_posix() + '/')
        with tempfile.TemporaryDirectory(prefix='eawr-projectile-proxy-') as temporary:
            directory = Path(temporary)
            cpp = directory / 'proxy_growth.cpp'
            exe = directory / ('proxy_growth.exe' if os.name == 'nt' else 'proxy_growth')
            cpp.write_text(harness)
            include_key = 'INCLUDE' if toolchain.msvc else 'CPLUS_INCLUDE_PATH'
            toolchain.environment[include_key] = str(ROOT / 'include') + os.pathsep + toolchain.environment.get(include_key, '')
            compile_fixture(toolchain, cpp, exe)
            run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_33_population_proxies_share_recipe_and_failure_keeps_old_visuals(self):
        source = (ROOT / 'apps/viewer/src/space_populate_arrivals.cpp').read_text()
        self.run_fixture(POPULATION.replace('__FUNCTIONS__', function(source,
                         'GodotRenderer::ProxyResult SpacePopulation::append_model_proxies(')))

    def test_fog_delay_and_ended_flights_do_not_request_slots(self):
        source = (ROOT / 'apps/viewer/src/battle_effects_projectiles.cpp').read_text()
        start = source.index('    std::map<std::string, std::vector<const tactical::Projectile*>> flying;')
        end = source.index('    const auto earlier = previous.projectiles();', start)
        self.run_fixture(ELIGIBILITY.replace('__LOOP__', source[start:end]))

    def test_backend_budget_failure_rollback_and_33_shared_proxies(self):
        toolchain = select_toolchain()
        if toolchain is None:
            if os.name == 'nt':
                self.fail('Windows contract requires the native MSVC fixture toolchain')
            self.skipTest('requires C++ compiler')
        source = (ROOT / 'src/presentation/godot/renderer_instances.cpp').read_text()
        harness = Path(__file__).with_name('projectile_proxy_harness.hpp').read_text()
        harness = harness.replace('__FUNCTIONS__', '\n'.join(function(source, signature) for signature in (
            'bool GodotRenderer::set_proxy_budget(', 'ResourceBudgetSnapshot GodotRenderer::proxy_budget(',
            'GodotRenderer::ProxyResult GodotRenderer::reserve_proxy_instances(',
            'void GodotRenderer::Impl::forget_instance_pose(',
            'GodotRenderer::Impl::InstanceMap::iterator GodotRenderer::Impl::remove_instance(')))
        # Includes resolve to the production accounting interface, not copies.
        self.run_fixture(harness)


if __name__ == '__main__':
    unittest.main()
