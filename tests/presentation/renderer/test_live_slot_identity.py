"""Launch-slot seed identity (#4218), without Godot.

Compile the production slot binding/release functions, the population identity binding (with the production idle
start hash) and the emitter seed override against small stand-ins. A source slot that returns early (a terminal
wreck) is reused at once, yet the unit that takes it still draws its particle seeds and idle start from the slot the
reservation policy would have bound it to. What is drawn follows that slot (the one BASE binds): when it and the
real slot differ in drawn state the unit binds it directly. The fallbacks are counted.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'tools'))
from native_cpp_fixture import compile_fixture, select_toolchain
sys.path.insert(0, str(Path(__file__).resolve().parent))
from viewer_mode_sources import source_text  # noqa: E402


def extract_function(source, signature):
    """Take an unindented function through its own closing brace."""
    start = source.index(signature)
    body = source.index('{', start)
    return source[start:source.index('\n}', body) + 2]


# Shared by both harnesses: the population with its production identity binding and idle start hash.
PREFIX = r'''
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sim { using EntityId = std::uint64_t; }
namespace scene { struct Placement { std::uint64_t scene_ordinal{}; std::uint32_t record_ordinal{}; std::string map_logical_path; }; }
struct Decision { std::uint64_t scene_ordinal{}; std::vector<int> hardpoints; };
struct Clip { std::uint32_t playable_frames() const { return 90; } };
struct IdlePlacement { std::size_t clip{}; std::uint32_t start_frame{}; };
int hash_calls = 0;
namespace animation {
__IDLE_START__
}
__PLACEMENT_START__
struct GodotRenderer {};
struct SpacePopulation {
    struct PlacedShip { sim::EntityId live_entity = 0; bool death_clone = false, projectile_slot = false; };
    struct { std::vector<PlacedShip> placed_ships; } options_;
    struct Scene { std::vector<scene::Placement> placements; };
    std::optional<Scene> scene_;
    std::vector<Decision> decisions_;
    std::vector<std::optional<std::size_t>> live_decisions_;
    std::vector<std::size_t> live_identity_;
    std::vector<IdlePlacement> idle_placements_;
    std::map<std::size_t, std::size_t> idle_live_ships_;
    std::vector<std::shared_ptr<const Clip>> clips_;
    int samples_scheduled = 0;
    void schedule_arrival_sample() noexcept { ++samples_scheduled; }
    struct LiveShipEmitterView { const scene::Placement* placement{}; std::vector<int> hardpoints; bool death_clone{}; bool projectile{};
                                 sim::EntityId entity{}; const scene::Placement* identity{}; };
    bool live_ship_drawn(std::size_t ship) const noexcept;
    std::optional<LiveShipEmitterView> live_ship_emitter_assets(std::size_t ship) const;
    const scene::Placement* live_placement(std::size_t ship) const noexcept;
    void bind_live_identity(std::size_t ship, std::size_t identity_ship);
    void restore_live_ship(GodotRenderer&, std::size_t) {}
};
using BattleScene = SpacePopulation;
__DRAWN__
__PLACEMENT__
__BIND__
__ASSETS__

// `ships` live ships: placements with scene ordinals scene_base.. and record ordinals record_base.., all drawn.
void fill(SpacePopulation& population, std::size_t ships, std::uint64_t scene_base, std::uint32_t record_base) {
    population.scene_.emplace();
    for (std::size_t ship = 0; ship < ships; ++ship) {
        population.scene_->placements.push_back({scene_base + ship, static_cast<std::uint32_t>(record_base + ship), "map"});
        population.decisions_.push_back({ship, {}});
        population.live_decisions_.push_back(ship);
        population.options_.placed_ships.push_back({900 + ship});
    }
    population.clips_.push_back(std::make_shared<Clip>());
}
'''

HARNESS = r'''
namespace sim {
namespace tactical {
using TypeId = std::uint32_t; using PlayerId = std::uint32_t;
struct TacticalInstance { EntityId entity_id{}; };
struct TacticalSnapshot {
    std::vector<TacticalInstance> rows;
    std::span<const TacticalInstance> instances() const { return rows; }
};
}
}
namespace tactical = sim::tactical;
namespace units { enum class UnitKind { ship, squadron }; struct UnitType { UnitKind kind = UnitKind::ship; std::vector<int> members; }; }
namespace live_session {
struct SlotIdentity final { std::size_t ship{}; bool held{}; };
}
struct DeathClone { std::size_t ship{}; };
struct ActiveClone { sim::EntityId unit{}; };
struct LiveSessionView {
    struct LaunchSlot { tactical::TypeId type{}; std::size_t ship{}; bool bound{}; std::optional<std::size_t> clone;
                        tactical::TypeId required_station{}; tactical::PlayerId station_owner{}; bool shadow_bound{}; };
    std::vector<LaunchSlot> launch_slots_;
    std::vector<DeathClone> launch_clones_;
    std::map<sim::EntityId, DeathClone> death_clones_;
    std::map<sim::EntityId, std::size_t> ship_of_entity_, launched_ship_of_entity_;
    std::map<sim::EntityId, live_session::SlotIdentity> slot_identity_of_entity_;
    std::set<sim::EntityId> terminal_source_leases_;
    std::vector<ActiveClone> active_clones_, retiring_clones_;
    std::map<std::pair<tactical::PlayerId, tactical::TypeId>, std::set<sim::EntityId>> slotless_hits_;
    std::map<tactical::TypeId, const units::UnitType*> presentation_types_;
    std::size_t slots_released_{}, identity_fallbacks_{}, identity_undrawn_{}, identity_both_undrawn_{}, shadow_held_{}, shadow_peak_{}, real_peak_{};
    struct { SpacePopulation* population{}; GodotRenderer* renderer{}; std::vector<sim::EntityId> undrawn;
             std::string failure; } arrival_slots_;
    std::optional<std::size_t> grow_launch_slot(sim::EntityId, tactical::TypeId, tactical::PlayerId) { return std::nullopt; }
    LaunchSlot& bind_slot_identity(sim::EntityId entity, LaunchSlot& real, tactical::TypeId type, tactical::PlayerId owner);
    void release_dead_slots(const tactical::TacticalSnapshot& latest);
    std::optional<std::size_t> ship_of(sim::EntityId entity, tactical::TypeId type, tactical::PlayerId owner);
};
__BIND_SLOT__
__RELEASE__
__SHIP_OF__

namespace {
// Three launch slots of one type (ships 10, 11, 12) among thirteen live ships.
LiveSessionView session(SpacePopulation& population, GodotRenderer& renderer) {
    fill(population, 13, 0, 100);
    LiveSessionView view;
    for (std::size_t index = 0; index < 3; ++index) {
        view.launch_slots_.push_back({7, 10 + index, false, std::nullopt, 0, 1, false});
    }
    view.arrival_slots_.population = &population;
    view.arrival_slots_.renderer = &renderer;
    return view;
}
tactical::TacticalSnapshot standing(std::initializer_list<sim::EntityId> entities) {
    tactical::TacticalSnapshot snapshot;
    for (const auto entity : entities) snapshot.rows.push_back({entity});
    return snapshot;
}
// Units 1 (terminal source, slot 10) and 2 (slot 11) launch; 1 retires, so slot 10 is physically free but its
// shadow slot stays reserved: the next unit's shadow pick is slot 12.
void retire_terminal_source(LiveSessionView& view) {
    view.ship_of(1, 7, 1); view.ship_of(2, 7, 1);
    view.terminal_source_leases_.insert(1);
    view.release_dead_slots(standing({2}));
}
}

int main() {
    GodotRenderer renderer;
    // Without early reuse the identity is always the real slot, and the fallbacks never fire.
    {
        SpacePopulation population; auto view = session(population, renderer);
        if (view.ship_of(1, 7, 1) != 10 || view.ship_of(2, 7, 1) != 11) return 1;
        view.release_dead_slots(standing({2}));  // 1 dies without a reservation: both pools free slot 10
        if (view.ship_of(3, 7, 1) != 10 || view.slot_identity_of_entity_.at(3).ship != 10 || view.identity_fallbacks_ != 0) return 2;
        for (std::size_t ship = 0; ship < population.live_identity_.size(); ++ship) if (population.live_identity_[ship] != ship) return 3;
        if (view.identity_undrawn_ != 0 || view.identity_both_undrawn_ != 0) return 4;
    }
    // A terminally retired source returns its real slot at once, but its shadow slot stays reserved: the next unit
    // reuses ship 10 and draws the identity of slot 12, the slot the reservation policy gives it.
    {
        SpacePopulation population; auto view = session(population, renderer);
        population.idle_live_ships_ = {{0, 10}};
        population.idle_placements_ = {{0, placement_start_frame(population.scene_->placements[10], 90)}};
        view.ship_of(1, 7, 1); view.ship_of(2, 7, 1);
        view.terminal_source_leases_.insert(1);
        view.release_dead_slots(standing({2}));
        if (view.launched_ship_of_entity_.contains(1) || view.slots_released_ != 1) return 10;
        if (view.ship_of(3, 7, 1) != 10) return 11;  // the real slot is reused
        const auto identity = view.slot_identity_of_entity_.at(3);
        if (identity.ship != 12 || !identity.held || population.live_identity_[10] != 12) return 12;
        // The idle clip of the reused slot starts where slot 12's does.
        if (population.idle_placements_[0].start_frame != placement_start_frame(population.scene_->placements[12], 90)) return 18;
        // A unit without a reservation frees both pools: slot 11 returns and is the next shadow slot too.
        view.release_dead_slots(standing({3}));
        if (view.ship_of(4, 7, 1) != 11 || view.slot_identity_of_entity_.at(4).ship != 11) return 13;
        if (view.identity_fallbacks_ != 0 || view.shadow_held_ != 3) return 14;
        // Shadow pool full (slot 10 reserved, 11 and 12 held): the real slot stands in, counted, not held.
        if (view.ship_of(5, 7, 1) != 12) return 15;  // the only real slot left
        const auto fallback = view.slot_identity_of_entity_.at(5);
        if (fallback.held || fallback.ship != view.launched_ship_of_entity_.at(5) || view.identity_fallbacks_ != 1 || view.shadow_held_ != 3) return 16;
        // Its release must not free a shadow slot it never held.
        view.release_dead_slots(standing({3, 4}));
        if (view.shadow_held_ != 3 || view.slot_identity_of_entity_.at(5).held) return 17;
    }
    // A spin-away craft leaves the snapshot without a death clone: it frees and rebinds every frame. The identity
    // entry is updated in place (same map node, no growth), the shadow slot comes back each time.
    {
        SpacePopulation population; auto view = session(population, renderer);
        if (view.ship_of(1, 7, 1) != 10) return 20;
        const auto* node = &view.slot_identity_of_entity_.at(1);
        for (int frame = 0; frame < 20; ++frame) {
            view.release_dead_slots(standing({}));
            if (view.ship_of(1, 7, 1) != 10) return 21;
            if (&view.slot_identity_of_entity_.at(1) != node || view.slot_identity_of_entity_.size() != 1) return 22;
            if (view.shadow_held_ != 1 || view.slot_identity_of_entity_.at(1).ship != 10) return 23;
        }
        if (view.identity_fallbacks_ != 0) return 24;
    }
    // The same spin on a reused slot (identity 12 on real slot 10): the production bind does no hash and schedules
    // no resample after the first frame, and the map keeps its node.
    {
        SpacePopulation population; auto view = session(population, renderer);
        population.idle_live_ships_ = {{0, 10}};
        population.idle_placements_ = {{0, placement_start_frame(population.scene_->placements[10], 90)}};
        retire_terminal_source(view);
        const int before = hash_calls;
        if (view.ship_of(3, 7, 1) != 10 || view.slot_identity_of_entity_.at(3).ship != 12) return 25;
        const int hashes = hash_calls, samples = population.samples_scheduled;
        if (hashes != before + 1 || samples != 1) return 26;  // the one rebind of slot 10 to identity 12
        const auto* node = &view.slot_identity_of_entity_.at(3);
        for (int frame = 0; frame < 20; ++frame) {
            view.release_dead_slots(standing({2}));
            if (view.ship_of(3, 7, 1) != 10) return 27;
            if (&view.slot_identity_of_entity_.at(3) != node || view.slot_identity_of_entity_.size() != 3) return 28;
            if (view.slot_identity_of_entity_.at(3).ship != 12 || view.shadow_held_ != 3) return 29;
        }
        if (hash_calls != hashes || population.samples_scheduled != samples) return 34;
    }
    // Shadow slot not drawn, real slot drawn: BASE binds the undrawn shadow slot and draws nothing, so the unit
    // binds the shadow slot itself and the real slot stays free. Counted apart from the shadow-empty fallback.
    {
        SpacePopulation population; auto view = session(population, renderer);
        retire_terminal_source(view);
        population.live_decisions_[12].reset();
        if (view.ship_of(3, 7, 1) != 12) return 30;
        const auto identity = view.slot_identity_of_entity_.at(3);
        if (!identity.held || identity.ship != 12 || view.launched_ship_of_entity_.at(3) != 12) return 31;
        if (view.identity_undrawn_ != 1 || view.identity_fallbacks_ != 0 || view.identity_both_undrawn_ != 0) return 32;
        if (view.launch_slots_[0].bound || !view.launch_slots_[2].bound || view.shadow_held_ != 3) return 33;
        view.release_dead_slots(standing({2}));  // its release frees slot 12 in both pools
        if (view.launch_slots_[2].bound || view.launch_slots_[2].shadow_bound || view.shadow_held_ != 2) return 35;
    }
    // Shadow slot drawn, real slot not drawn: BASE draws the unit on the shadow slot, so it must not be hidden
    // on the undrawn real slot.
    {
        SpacePopulation population; auto view = session(population, renderer);
        retire_terminal_source(view);
        population.live_decisions_[10].reset();
        if (view.ship_of(3, 7, 1) != 12) return 40;
        const auto identity = view.slot_identity_of_entity_.at(3);
        if (!identity.held || identity.ship != 12 || !population.live_ship_drawn(12)) return 41;
        if (view.identity_undrawn_ != 1 || view.identity_fallbacks_ != 0 || view.launch_slots_[0].bound) return 42;
    }
    // Neither slot drawn: nothing shows either way, the real slot is reused with the shadow identity; counted.
    {
        SpacePopulation population; auto view = session(population, renderer);
        retire_terminal_source(view);
        population.live_decisions_[10].reset(); population.live_decisions_[12].reset();
        if (view.ship_of(3, 7, 1) != 10) return 50;
        const auto identity = view.slot_identity_of_entity_.at(3);
        if (!identity.held || identity.ship != 12) return 51;
        if (view.identity_both_undrawn_ != 1 || view.identity_undrawn_ != 0 || view.identity_fallbacks_ != 0) return 52;
    }
    // A terminal source that re-enters ship_of keeps its held shadow slot and takes no second one.
    {
        SpacePopulation population; auto view = session(population, renderer);
        retire_terminal_source(view);
        if (view.ship_of(3, 7, 1) != 10) return 60;  // unit 3 takes the freed real slot 10, shadow 12
        if (view.shadow_held_ != 3) return 61;
        if (view.ship_of(1, 7, 1) != 12) return 62;  // unit 1 returns: the only real slot left
        const auto identity = view.slot_identity_of_entity_.at(1);
        if (!identity.held || identity.ship != 10 || population.live_identity_[12] != 10) return 63;
        if (view.shadow_held_ != 3 || view.identity_fallbacks_ != 0 || view.launch_slots_[2].shadow_bound != true) return 64;
    }
    std::cout << "reused source slot draws the reservation-policy identity; drawn state follows the shadow slot; fallbacks counted\n";
    return 0;
}
'''

POPULATION_HARNESS = r'''
__SEED__

int main() {
    SpacePopulation population;
    // Four live ships: placements with scene ordinals 40..43 and record ordinals 70..73.
    fill(population, 4, 40, 70);
    population.idle_placements_ = {{0, placement_start_frame(population.scene_->placements[1], 90)}};
    population.idle_live_ships_ = {{0, 1}};
    // An unreused ship is its own identity.
    auto own = population.live_ship_emitter_assets(1);
    if (!own || own->identity != own->placement) return 1;
    // Ship 1 now carries the identity of slot 3: the emitter view hands out that placement's ordinals, the
    // ship's own placement and entity stay the real ones, and the idle clip starts where slot 3's does.
    population.bind_live_identity(1, 3);
    auto reused = population.live_ship_emitter_assets(1);
    if (!reused || reused->placement != &population.scene_->placements[1] || reused->identity != &population.scene_->placements[3]
        || reused->entity != 901) return 2;
    if (population.idle_placements_[0].start_frame != placement_start_frame(population.scene_->placements[3], 90)
        || population.samples_scheduled != 1) return 3;
    // The same identity again (a spin-away craft rebinds each frame) is a no-op: no hash, no resample.
    const int calls = hash_calls;
    for (int frame = 0; frame < 20; ++frame) population.bind_live_identity(1, 3);
    if (hash_calls != calls || population.samples_scheduled != 1) return 8;
    // The other ships keep their own identity; a later rebind to the own slot restores the own idle start.
    auto other = population.live_ship_emitter_assets(2);
    if (!other || other->identity != other->placement) return 4;
    population.bind_live_identity(1, 1);
    if (population.live_ship_emitter_assets(1)->identity != &population.scene_->placements[1]
        || population.idle_placements_[0].start_frame != placement_start_frame(population.scene_->placements[1], 90)) return 5;
    // The seed override: the root placement takes the identity's scene and record ordinals, an attached model keeps
    // its own record ordinal and only takes the scene ordinal.
    population.bind_live_identity(1, 3);
    const auto view = *population.live_ship_emitter_assets(1);
    scene::Placement root = *view.placement, attached = *view.placement;
    attached.record_ordinal = 5;
    apply_seed_identity(root, view, true);
    apply_seed_identity(attached, view, false);
    if (root.scene_ordinal != 43 || root.record_ordinal != 73 || attached.scene_ordinal != 43 || attached.record_ordinal != 5) return 6;
    // Without an identity (a view built elsewhere) nothing changes.
    scene::Placement plain = *view.placement;
    SpacePopulation::LiveShipEmitterView bare = view; bare.identity = nullptr;
    apply_seed_identity(plain, bare, true);
    if (plain.scene_ordinal != 41 || plain.record_ordinal != 71) return 7;
    std::cout << "population identity and seed override\n";
    return 0;
}
'''


def population_prefix():
    animation = source_text('apps/viewer/src/space_populate_animation.cpp')
    idle = source_text('src/presentation/animation/idle_playback.cpp')
    clips = source_text('apps/viewer/src/idle_clips.hpp')
    hash_function = extract_function(idle, 'std::uint32_t idle_start_frame(').replace(
        'if (frames == 0) return 0;', 'if (frames == 0) return 0;\n    ++hash_calls;', 1)
    return (PREFIX
            .replace('__IDLE_START__', hash_function)
            .replace('__PLACEMENT_START__', extract_function(clips, 'inline std::uint32_t placement_start_frame('))
            .replace('__DRAWN__', extract_function(animation, 'bool SpacePopulation::live_ship_drawn('))
            .replace('__PLACEMENT__', extract_function(animation, 'const scene::Placement* SpacePopulation::live_placement('))
            .replace('__BIND__', extract_function(animation, 'void SpacePopulation::bind_live_identity('))
            .replace('__ASSETS__', extract_function(
                animation, 'std::optional<SpacePopulation::LiveShipEmitterView> SpacePopulation::live_ship_emitter_assets(')))


class LiveSlotIdentity(unittest.TestCase):
    def _run(self, name, text):
        toolchain = select_toolchain()
        if toolchain is None:
            self.skipTest('requires C++ compiler')
        with tempfile.TemporaryDirectory(prefix='eawr-slot-identity-') as temp:
            directory = Path(temp)
            cpp = directory / (name + '.cpp')
            exe = directory / (name + ('.exe' if os.name == 'nt' else ''))
            cpp.write_text(text, encoding='utf-8')
            compile_fixture(toolchain, cpp, exe)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=10)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)

    def test_reused_source_slot_keeps_the_reservation_identity(self):
        source = source_text('apps/viewer/src/live_session_fighters.cpp')
        text = (population_prefix() + HARNESS
                .replace('__BIND_SLOT__', extract_function(source, 'LiveSessionView::LaunchSlot& LiveSessionView::bind_slot_identity('))
                .replace('__RELEASE__', extract_function(source, 'void LiveSessionView::release_dead_slots('))
                .replace('__SHIP_OF__', extract_function(source, 'std::optional<std::size_t> LiveSessionView::ship_of(')))
        self._run('slot_identity', text)

    def test_population_identity_and_emitter_seed_override(self):
        prepare = source_text('apps/viewer/src/unit_emitters_prepare.cpp')
        text = population_prefix() + POPULATION_HARNESS.replace('__SEED__', extract_function(prepare, 'void apply_seed_identity('))
        self._run('population_identity', text)


if __name__ == '__main__':
    unittest.main()
