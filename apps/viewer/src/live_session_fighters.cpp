#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "live_session_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

bool LiveSessionView::squadron_launched(const sim::EntityId container) const {
    // A squadron the setup does not list came out of a hangar (#518); with no setup to compare
    // with, none is known to have (the least visible answer).
    return setup_ && squadrons_.contains(container)
        && std::none_of(setup_->squadrons.begin(), setup_->squadrons.end(),
                        [&](const tactical::Squadron& start) { return start.container == container; });
}

void LiveSessionView::prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    // Past the debris props (max / 2 down) and the death clones (max down).
    constexpr sim::EntityId first_slot_entity = std::numeric_limits<sim::EntityId>::max() / 4U;
    if (!start_ || !tables_) return;
    const auto add_slot = [&](const std::string& model, const skirmish::StartUnit& near,
                              const std::optional<skirmish::LobbyColour>& colour,
                              const tactical::TypeId required_station = 0) {
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = model;
        ship.position = {to_float(near.state.position.x), to_float(near.state.position.y), to_float(near.state.position.z)};
        ship.yaw_degrees = to_float(near.yaw_degrees);
        ship.live_entity = first_slot_entity + launch_slots_.size();
        ship.launch_slot = true;
        if (const auto* type = tables_->find(model); type && type->under_construction) {
            ship.construction = true;
            if (auto object = catalog.resolve(type->id, data::Category::game_object); object) {
                const auto paths = animation::model_clip_paths(space_model_path(object.value()),
                    *animation::clip_type_index("BUILD"),
                    [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); },
                    tag_text(object.value(), "Space_Model_Anim_Override_Name"));
                if (!paths.empty()) ship.clip = paths.front();
            }
        }
        if (colour) ship.team_colour = colour->rgb;
        launch_slots_.push_back({skirmish::type_id(model), placed_ships_.size(), false, std::nullopt,
                                 required_station, near.state.owner});
        placed_ships_.push_back(std::move(ship));
    };
    // WBP-29: one reusable replacement slot per initially placed respawnable object.
    for (const auto& unit : start_->units) {
        if (economy_.pads.replacement(unit.state.type_id) == nullptr) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& player) { return player.player.player_id == unit.state.owner; });
        add_slot(unit.type, unit, owner != start_->players.end() ? owner->colour : std::optional<skirmish::LobbyColour>{});
    }
    for (const skirmish::Launch& launch : start_->launches) {
        const units::UnitType* squadron = tables_->find(launch.squadron);
        const auto spawner = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.entity_id == launch.spawner; });
        if (squadron == nullptr || spawner == start_->units.end() || launch.count <= 0 || launch.count > 64) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == launch.owner; });
        const std::optional<skirmish::LobbyColour> colour =
            owner != start_->players.end() ? owner->colour : std::optional<skirmish::LobbyColour>{};
        for (std::int32_t index = 0; index < launch.count; ++index) {
            for (const units::SquadronMember& member : squadron->members) add_slot(member.craft, *spawner, colour);
        }
    }
    // PU-10/SAE-03: purchased units enter after tick zero, so both human and AI players get slots
    // composed with the start: per available unit of its faction's menus, as many as fit its
    // population cap, at most purchase_slots_per_type (space-purchasing PU-G25). Each slot is the
    // unit's model, or for a squadron each craft's. A dead
    // unit's slot is reused (release_dead_slots), so the count bounds what stands at once, not how
    // many were ever bought.
    const std::uint32_t purchase_slots_per_type = options_.purchase_slots;
    // WBP-13/18: UC and final types occupy a live slot immediately. Bound the reserved slots
    // by map pads offering that type, rather than reinforcement population or purchase queues.
    for (const auto& builder : start_->players) {
        if (!builder.lobby) continue;
        const auto near = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.owner == builder.player.player_id; });
        if (near == start_->units.end()) continue;
        std::map<tactical::TypeId, std::size_t> slots;
        for (const auto& menu : economy_.menus) {
            if (menu.faction != builder.player.faction_id || economy_.pads.point(menu.station) == nullptr) continue;
            const auto count = std::count_if(start_->units.begin(), start_->units.end(),
                [&](const skirmish::StartUnit& unit) { return unit.state.type_id == menu.station; });
            for (const auto& option : menu.options) {
                const auto* child = economy_.pads.child(option.type);
                if (!option.available || child == nullptr) continue;
                slots[child->type] += static_cast<std::size_t>(count);
                slots[child->constructed] += static_cast<std::size_t>(count);
            }
        }
        for (const auto& [type, count] : slots) {
            const auto named = std::find_if(tables_->units.begin(), tables_->units.end(),
                [&](const units::UnitType& unit) { return skirmish::type_id(unit.id) == type; });
            if (named == tables_->units.end()) continue;
            for (std::size_t index = 0; index < count; ++index) add_slot(named->id, *near, builder.colour);
        }
    }
    for (const tactical::EconomyPlayer& buyer : economy_.players) {
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == buyer.player; });
        const auto near = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.owner == buyer.player; });
        if (owner == start_->players.end() || near == start_->units.end()) continue;
        std::vector<tactical::TypeId> seen;
        std::vector<tactical::TypeId> station_slots;
        for (const tactical::StationMenu& menu : economy_.menus) {
            if (menu.faction != owner->player.faction_id) continue;
            // WPR-52: station replacements use the same preloaded-slot path as arrivals.
            if (menu.next_level != 0 && std::find(station_slots.begin(), station_slots.end(), menu.next_level) == station_slots.end()) {
                station_slots.push_back(menu.next_level);
                const auto next = std::find_if(tables_->units.begin(), tables_->units.end(),
                    [&](const units::UnitType& type) { return skirmish::type_id(type.id) == menu.next_level; });
                if (next != tables_->units.end()) {
                    add_slot(next->id, *near, owner->colour);
                    // WPR-52/SK-23: the replacement's larger hangar can launch craft absent
                    // from the initial station. Preload its authored simultaneous complement.
                    if (next->spawner) for (const auto& entry : next->spawner->starting) {
                        const auto* squadron = tables_->find(entry.squadron);
                        if (squadron == nullptr || entry.count <= 0 || entry.count > 64) continue;
                        for (std::int32_t index = 0; index < entry.count; ++index) {
                            for (const auto& member : squadron->members) add_slot(member.craft, *near, owner->colour, menu.next_level);
                        }
                    }
                }
            }
            for (const tactical::BuildOption& option : menu.options) {
                if (!option.available || option.kind != tactical::BuildKind::unit) continue;
                if (std::find(seen.begin(), seen.end(), option.type) != seen.end()) continue;
                seen.push_back(option.type);
                // WHE-49/50: companies retain their purchase identity, while their
                // preview and arrival slots use the deployed hull or squadron.
                const auto* hero = economy_.hero(option.type);
                const auto deployed = hero ? hero->deployed : option.type;
                const auto named = std::find_if(tables_->units.begin(), tables_->units.end(),
                    [&](const units::UnitType& type) { return skirmish::type_id(type.id) == deployed; });
                if (named == tables_->units.end()) continue;
                // WR-12/13: separate preloaded visual clones, never launch slots or live entities.
                if (buyer.player == player_) {
                    const auto add_preview = [&](const std::string& craft, const sim::math::Vec3 offset) {
                        SpacePopulation::Options::PlacedShip preview;
                        preview.object_id = craft;
                        preview.live_entity = std::numeric_limits<sim::EntityId>::max() / 8U + placement_clones_.size();
                        preview.placement_preview = true;
                        preview.launch_slot = true; // starts hidden until an explicit preview pose
                        if (owner->colour) preview.team_colour = owner->colour->rgb;
                        const auto* footprint = economy_.footprint_of(skirmish::type_id(craft));
                        placement_clones_.push_back({option.type, placed_ships_.size(), offset, footprint ? footprint->layer_z : sim::math::Fixed{}});
                        placed_ships_.push_back(std::move(preview));
                    };
                    if (named->members.empty()) add_preview(named->id, {});
                    for (const auto& member : named->members) add_preview(member.craft, member.offset.value_or(sim::math::Vec3{}));
                }
                const std::uint32_t fits = option.population == 0 ? purchase_slots_per_type : buyer.population_cap / option.population;
                for (std::uint32_t index = 0; index < std::min(fits, purchase_slots_per_type); ++index) {
                    if (named->members.empty()) add_slot(named->id, *near, owner->colour);
                    for (const units::SquadronMember& member : named->members) add_slot(member.craft, *near, owner->colour);
                }
            }
        }
    }
    // #458: each slot's death clone, set up like a start unit's (the clones used to be planned
    // for the start units only, so a craft launched after tick zero died without one). The
    // clone's clip variant is drawn from the slot's own entity, as the craft's ID is not known
    // until it launches.
    for (LaunchSlot& slot : launch_slots_) {
        if (auto clone = prepare_death_clone(filesystem, catalog, slot.ship, placed_ships_[slot.ship].live_entity)) {
            slot.clone = launch_clones_.size();
            launch_clones_.push_back(std::move(*clone));
        }
    }
}

void LiveSessionView::register_squadron(const tactical::Squadron& squadron, const std::uint64_t tick) {
    // WHE-SQ-02: the solo fighter's self-represented flight group remains one ordinary visible
    // unit, retaining its shield bar, selection and hero portrait.
    if (squadron.members.size() == 1 && squadron.members.front() == squadron.container) return;
    auto [entry, inserted] = squadrons_.try_emplace(squadron.container, squadron);
    if (!inserted) {
        // WHE-63: a retained team can gain replacement craft. Frames read both
        // snapshot ends; never restore an older roster on the next drawn frame.
        if (entry->second.members == squadron.members
            || (battle_frame_.latest && tick < battle_frame_.latest->completed_tick())) return;
        for (const auto member : entry->second.members) {
            const auto owner = squadron_of_.find(member);
            if (owner != squadron_of_.end() && owner->second == squadron.container) squadron_of_.erase(owner);
        }
        entry->second = squadron;
    }
    squadron_seen_.emplace(squadron.container, tick);
    squadron_members_[squadron.container] = squadron.members;
    for (const sim::EntityId member : squadron.members) squadron_of_[member] = squadron.container;
}

void LiveSessionView::release_dead_slots(const tactical::TacticalSnapshot& latest) {
    const auto instances = latest.instances();
    const auto standing = [&](const sim::EntityId entity) {
        const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
            [](const tactical::TacticalInstance& instance, const sim::EntityId id) { return instance.entity_id < id; });
        return found != instances.end() && found->entity_id == entity;
    };
    const auto cloned = [&](const sim::EntityId entity) {
        const auto is = [&](const ActiveClone& clone) { return clone.unit == entity; };
        return std::any_of(active_clones_.begin(), active_clones_.end(), is)
            || std::any_of(retiring_clones_.begin(), retiring_clones_.end(), is);
    };
    for (auto bound = launched_ship_of_entity_.begin(); bound != launched_ship_of_entity_.end();) {
        if (standing(bound->first) || cloned(bound->first)) {
            ++bound;
            continue;
        }
        for (LaunchSlot& slot : launch_slots_) {
            if (slot.ship == bound->second) slot.bound = false;
        }
        death_clones_.erase(bound->first);
        ++slots_released_;
        bound = launched_ship_of_entity_.erase(bound);
    }
}

std::optional<std::size_t> LiveSessionView::ship_of(const sim::EntityId entity, const tactical::TypeId type,
                                                const tactical::PlayerId owner) {
    if (const auto found = ship_of_entity_.find(entity); found != ship_of_entity_.end()) return found->second;
    if (const auto found = launched_ship_of_entity_.find(entity); found != launched_ship_of_entity_.end()) return found->second;
    for (LaunchSlot& slot : launch_slots_) {
        // The model and its death clone were composed with this owner's lobby colour.
        if (slot.bound || slot.type != type || slot.station_owner != owner || slot.required_station != 0) continue;
        slot.bound = true;
        launched_ship_of_entity_.emplace(entity, slot.ship);
        if (slot.clone) death_clones_.emplace(entity, launch_clones_[*slot.clone]);
        return slot.ship;
    }
    return std::nullopt;
}

const tactical::Squadron* LiveSessionView::squadron_of(const sim::EntityId craft) const noexcept {
    const auto container = squadron_of_.find(craft);
    if (container == squadron_of_.end()) return nullptr;
    const auto squadron = squadrons_.find(container->second);
    return squadron == squadrons_.end() ? nullptr : &squadron->second;
}

} // namespace eawr::presentation::godot_backend
