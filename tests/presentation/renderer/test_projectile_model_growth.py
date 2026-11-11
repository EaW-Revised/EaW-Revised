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
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include "eawr/presentation/proxy_reservation.hpp"
using namespace eawr::presentation;
namespace sim { using EntityId = std::uint64_t; using AssetId = std::uint64_t; }
namespace scene {
struct Placement { std::size_t scene_ordinal{}; sim::EntityId entity_id{}; std::uint32_t record_ordinal{}; };
struct SpacePlacementDecision { std::size_t scene_ordinal{}; };
struct Scene { std::vector<Placement> placements; };
}
namespace space {
struct PopulationIndex {
    std::vector<std::vector<std::size_t>> rows;
    std::span<const std::size_t> pieces(std::size_t ship) const { return rows.at(ship); }
    std::size_t ship_count() const { return rows.size(); }
    void truncate(std::size_t ships) { if (rows.size() > ships) rows.resize(ships); }
};
struct AttachmentMarks {
    std::size_t size{};
    template<class T> void extend(const T& states) { size = states.size(); }
    std::size_t decisions() const { return size; }
    void truncate(std::size_t decisions) { if (size > decisions) size = decisions; }
};
}
std::uint32_t placement_start_frame(const scene::Placement& placement, const std::uint32_t frames) {
    return placement.record_ordinal % frames;
}
template<class Vector> void reserve_geometric(Vector& values, const std::size_t wanted) {
    if (wanted > values.capacity()) values.reserve(std::max(wanted, values.capacity() * 2));
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
    struct Ship { std::string object_id; bool projectile_slot{}; sim::EntityId live_entity{};
        bool launch_slot{}, death_clone{}; };
    struct Options { using PlacedShip = Ship; std::vector<Ship> placed_ships; } options_;
    struct Piece { struct { sim::EntityId entity_id{}; sim::AssetId asset_id{}; } instance;
        std::optional<std::size_t> live; struct Gate { std::size_t decision{}; }; std::optional<Gate> gate; };
    using GatedInstance = Piece;
    struct LiveClipInstance { std::size_t ship{}; sim::EntityId entity{}; sim::AssetId asset{}; };
    struct AnimatedInstance { sim::EntityId entity{}; sim::AssetId asset{}; std::size_t idle{}; };
    std::vector<Piece> pieces_;
    std::vector<LiveClipInstance> live_clip_instances_;
    std::vector<AnimatedInstance> animated_instances_;
    struct IdlePlacement { std::size_t clip{}; std::uint32_t start_frame{}; };
    std::vector<IdlePlacement> idle_placements_;
    struct Clip { int value{}; std::uint32_t playable_frames() const { return 90; } };
    std::vector<std::shared_ptr<const Clip>> clips_;
    std::size_t map_placements_{};
    std::map<std::size_t, std::size_t> idle_live_ships_;
    struct TurretBinding { std::optional<std::size_t> idle, applied; };
    std::vector<std::optional<TurretBinding>> turret_bindings_, parent_hardpoint_turret_bindings_;
    std::vector<std::map<std::uint32_t, TurretBinding>> hardpoint_turret_bindings_;
    std::vector<std::vector<int>> contact_bones_;
    std::vector<bool> live_death_clone_, backdrop_ships_;
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
        turret_bindings_.resize(size); parent_hardpoint_turret_bindings_.resize(size); hardpoint_turret_bindings_.resize(size);
        contact_bones_.resize(size); live_death_clone_.resize(size); backdrop_ships_.resize(size);
    }
    void rebuild_piece_index() {
        piece_index_.rows.assign(options_.placed_ships.size(), {});
        for (std::size_t index = 0; index < pieces_.size(); ++index) piece_index_.rows[*pieces_[index].live].push_back(index);
    }
    void append_piece_index(std::size_t first_piece, std::size_t) {
        piece_index_.rows.resize(options_.placed_ships.size());
        for (std::size_t index = first_piece; index < pieces_.size(); ++index) piece_index_.rows[*pieces_[index].live].push_back(index);
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
    population.idle_placements_.resize(10);
    population.idle_live_ships_.emplace(9, 0);
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
        assert(population.animated_instances_[index].idle == 9 + index);
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
    def run_fixture(self, harness, expected_success=True):
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
            if expected_success:
                self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            else:
                self.assertNotEqual(run.returncode, 0, "mutant unexpectedly passed")

    def test_shared_idle_detach_restores_exact_bones(self):
        source = (ROOT / 'src/presentation/godot/renderer_instances.cpp').read_text()
        harness = Path(__file__).with_name('shared_idle_skeleton_harness.hpp').read_text()
        header = (ROOT / 'src/presentation/godot/renderer_internal.hpp').read_text()
        start = header.index('    static void apply_visibility(')
        harness = harness.replace('__VISIBILITY__', header[start:header.index('\n    }', start) + 6])
        harness = harness.replace('__FUNCTIONS__', '\n'.join(function(source, signature) for signature in (
            'GodotRenderer::Impl::SharedIdleSkeleton::~SharedIdleSkeleton(',
            'void GodotRenderer::Impl::prepare_shared_idle_surface(',
            'void GodotRenderer::Impl::release_instance_skeleton(',
            'void GodotRenderer::Impl::detach_shared_idle(',
            'core::Result<void> GodotRenderer::Impl::bind_skin_pose(',
            '[[nodiscard]] bool GodotRenderer::Impl::apply_skin_pose(',
            'void GodotRenderer::Impl::bind_instance_skin(',
            'void GodotRenderer::Impl::clear_skin_pose(',
            'void GodotRenderer::Impl::set_instance_visible(',
            'GodotRenderer::Impl::InstanceMap::iterator GodotRenderer::Impl::remove_instance(')))
        self.run_fixture(harness)
        self.run_fixture("#define NDEBUG 1\n" + harness)
        mutants = {
            "last_owner_free": "if (auto* rendering = RenderingServer::get_singleton(); rendering && rid.is_valid()) rendering->free_rid(rid);",
            "detach_attach": "rendering.instance_attach_skeleton(instance.rid, replacement);",
            "join_attach": "shared_rendering->instance_attach_skeleton(existing->second.rid, shared->second->rid);",
        }
        for name, statement in mutants.items():
            with self.subTest(mutant=name):
                self.assertEqual(harness.count(statement), 1)
                self.run_fixture(harness.replace(statement, "/* mutant */", 1), expected_success=False)

    def test_33_population_proxies_share_recipe_and_failure_keeps_old_visuals(self):
        source = (ROOT / 'apps/viewer/src/space_populate_arrivals.cpp').read_text()
        self.run_fixture(POPULATION.replace('__FUNCTIONS__', function(source,
                         'GodotRenderer::ProxyResult SpacePopulation::append_model_proxies(')))

    def test_launch_and_clone_copies_preserve_private_animation_and_rollback(self):
        source = (ROOT / 'apps/viewer/src/space_populate_arrivals.cpp').read_text()
        harness = POPULATION[:POPULATION.index('int main() {')] + r'''int main() {
    GodotRenderer renderer;
    SpacePopulation population;
    population.options_.placed_ships.push_back({"mod_hangar_member", false, 100, true, false});
    population.prepare_compose_slots(true);
    population.live_decisions_[0] = 0;
    population.scene_->placements.push_back({0, 1});
    population.decisions_.push_back({0});
    population.hardpoint_states_.push_back({3});
    population.pieces_.push_back({{10, 77}, 0, {{0}}});
    population.live_clip_instances_.push_back({0, 10, 77});
    population.animated_instances_.push_back({10, 77, 0});
    population.idle_placements_.push_back({0});
    population.idle_live_ships_.emplace(0, 0);
    population.contact_bones_[0] = {6, 7};
    population.clips_ = {std::make_shared<const SpacePopulation::Clip>(SpacePopulation::Clip{77}),
        std::make_shared<const SpacePopulation::Clip>(SpacePopulation::Clip{88})};
    population.live_clips_[0] = 0;
    population.live_alternate_clips_[0] = 1;
    population.turret_bindings_[0] = SpacePopulation::TurretBinding{0, 9};
    population.parent_hardpoint_turret_bindings_[0] = SpacePopulation::TurretBinding{0, 9};
    population.hardpoint_turret_bindings_[0].emplace(2, SpacePopulation::TurretBinding{0, 9});
    population.rebuild_piece_index();
    std::vector<SpacePopulation::Ship> ships(2, {"mod_hangar_member", false, 101, true, false});
    renderer.outcome = ProxyOutcome::allocation_failed;
    auto failed = population.append_model_proxies(renderer, 0, ships);
    assert(failed.outcome == ProxyOutcome::allocation_failed);
    assert(population.idle_placements_.size() == 1 && population.idle_live_ships_.size() == 1);
    assert(population.options_.placed_ships.size() == 1 && population.contact_bones_.size() == 1);
    renderer.outcome = ProxyOutcome::reserved;
    auto result = population.append_model_proxies(renderer, 0, ships);
    assert(result.outcome == ProxyOutcome::reserved && result.first_slot == 1);
    for (std::size_t ship = 1; ship < 3; ++ship) {
        assert(population.animated_instances_[ship].idle == ship);
        assert(population.idle_live_ships_.at(ship) == ship);
        assert(population.scene_->placements[ship].record_ordinal == ship);
        assert(population.idle_placements_[ship].start_frame == ship);
        assert(population.turret_bindings_[ship]->idle == ship && !population.turret_bindings_[ship]->applied);
        assert(population.parent_hardpoint_turret_bindings_[ship]->idle == ship);
        assert(population.hardpoint_turret_bindings_[ship].at(2).idle == ship);
        assert(population.pieces_[ship].gate->decision == ship);
        assert(population.live_clip_instances_[ship].ship == ship);
        assert(population.contact_bones_[ship] == population.contact_bones_[0]);
        assert(population.live_clips_[ship] != population.live_clips_[0]);
        assert(population.live_alternate_clips_[ship] != population.live_alternate_clips_[0]);
        assert(population.clips_[*population.live_clips_[ship]]->value == 77);
    }
    population.clips_[*population.live_clips_[1]].reset();
    assert(population.clips_[*population.live_clips_[2]]->value == 77 && population.clips_[0]->value == 77);
    population.contact_bones_[1][0] = 99;
    assert(population.contact_bones_[0][0] == 6);
    population.options_.placed_ships[0].launch_slot = false;
    population.options_.placed_ships[0].death_clone = true;
    std::vector<SpacePopulation::Ship> wrecks{{"mod_hangar_member", false, 999, false, true}};
    result = population.append_model_proxies(renderer, 0, wrecks);
    assert(result.outcome == ProxyOutcome::reserved && population.live_death_clone_[result.first_slot]);
}
'''
        self.run_fixture(harness.replace('__FUNCTIONS__', function(source,
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
