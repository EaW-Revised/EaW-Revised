#include "eawr/battle/start.hpp"
#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"
#include "campaign_warm_arrivals.hpp"
#include "reserve_warm_plan.hpp"
#include "eawr/presentation/godot/device_budget.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/live/live_units.hpp"
#include "eawr/presentation/live/unit_fade.hpp"
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
    // WU-37: use published launch provenance, independent of when the team first appeared.
    if (!battle_frame_.latest) return false;
    const auto instances = battle_frame_.latest->instances();
    const auto found = std::lower_bound(instances.begin(), instances.end(), container,
        [](const tactical::TacticalInstance& instance, const sim::EntityId id) { return instance.entity_id < id; });
    return found != instances.end() && found->entity_id == container && found->garrison_unit;
}

void LiveSessionView::add_launch_slot(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const std::string& model, const battle::StartUnit& near,
    const std::optional<battle::LobbyColour>& colour, const tactical::TypeId required_station,
    const bool ability_proxy_owner) {
    constexpr sim::EntityId first_slot_entity = std::numeric_limits<sim::EntityId>::max() / 4U;
    BattleScene::PlacedUnit ship;
    ship.object_id = model;
    ship.position = {to_float(near.state.position.x), to_float(near.state.position.y), to_float(near.state.position.z)};
    ship.yaw_degrees = to_float(near.yaw_degrees);
    ship.live_entity = first_slot_entity + launch_slots_.size();
    ship.launch_slot = true;
    ship.ability_proxy_owner = ability_proxy_owner;
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
    if (!colorization_enabled_) ship.team_colour = model_colour(near.state.owner, model);
    else if (colour) ship.team_colour = colour->rgb;
    launch_slots_.push_back({battle::type_id(model), placed_ships_.size(), false, std::nullopt,
                             required_station, near.state.owner, false});
    placed_ships_.push_back(std::move(ship));
}

void LiveSessionView::prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                           std::optional<battle::ArrivalPlan> selected) {
    if (!start_ || !tables_ || !selected) return;
    arrival_slots_.filesystem = &filesystem;
    arrival_slots_.catalog = &catalog;
    arrival_slots_.plan = std::move(*selected);
    const bool campaign = !options_.land && setup_ && setup_->campaign;
    if (campaign && motion()) {
        // The sim's selected owner-tech profiles include hangars on carriers
        // still in reserve. start.launches alone names only the initial force.
        std::map<tactical::TypeId, const units::UnitType*> names;
        for (const auto& type : tables_->units) names.emplace(battle::type_id(type.id), &type);
        std::vector<CampaignWarmType> types;
        for (const auto& player : start_->players) for (const auto& type : tables_->units) {
            const auto* profile = motion()->squadrons.find_spawner(battle::type_id(type.id), player.player.player_id);
            if (!profile) continue;
            CampaignWarmType warm{player.player.player_id, type.id, {}};
            for (const auto& entry : profile->entries) {
                if (entry.starting <= 0) continue;
                const auto named = names.find(entry.squadron);
                if (named == names.end()) { arrival_slots_.warm_complete = false; continue; }
                const auto count = static_cast<std::size_t>(entry.starting);
                if (named->second->members.empty()) warm.hangar.emplace_back(named->second->id, count);
                for (const auto& member : named->second->members) warm.hangar.emplace_back(member.craft, count);
            }
            types.push_back(std::move(warm));
        }
        std::vector<CampaignWarmSeed> seeds;
        const auto seed = [&](const tactical::PlayerId owner, const tactical::TypeId type, const sim::EntityId anchor) {
            const auto* hero = economy_.hero(type);
            const auto found = names.find(hero ? hero->deployed : type);
            if (found != names.end()) seeds.push_back({owner, found->second->id, 1, anchor});
        };
        for (const auto& unit : start_->units) seed(unit.state.owner, unit.state.type_id, unit.state.entity_id);
        const auto carried = [&](auto&& self, const tactical::PlayerId owner,
            const tactical::CarriedObject& object) -> void {
            seed(owner, object.type_id, 0);
            for (const auto& member : object.members) self(self, owner, member);
        };
        for (const auto& reserve : setup_->campaign->reserves) {
            if (reserve.restricted) continue;
            seed(reserve.owner, reserve.type, 0);
            for (const auto& object : reserve.contained) carried(carried, reserve.owner, object);
        }
        auto expanded = campaign_warm_arrivals(seeds, types);
        arrival_slots_.warm_complete = arrival_slots_.warm_complete && expanded.complete;
        std::erase_if(arrival_slots_.plan.rows, [](const auto& row) { return row.source == battle::ArrivalSource::launch; });
        for (auto& row : expanded.plan.rows) arrival_slots_.plan.rows.push_back(std::move(row));
    }
    // WSS-54/61: every preparation path needs an owner anchor, even when a teammate
    // owns the shared station and this player fields no starting units.
    std::map<tactical::PlayerId, battle::StartUnit> anchors;
    for (const auto& owner : start_->players) {
        const auto near = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const auto& unit) { return unit.state.owner == owner.player.player_id; });
        battle::StartUnit anchor{};
        if (near != start_->units.end()) anchor = *near;
        else {
            const auto allied = [&](const battle::StartUnit& unit) {
                return std::any_of(start_->players.begin(), start_->players.end(), [&](const auto& player) {
                    return player.player.player_id == unit.state.owner
                        && player.player.team_id == owner.player.team_id;
                });
            };
            auto shared = std::find_if(start_->units.begin(), start_->units.end(), [&](const auto& unit) {
                return allied(unit) && std::any_of(economy_.menus.begin(), economy_.menus.end(), [&](const auto& menu) {
                    return menu.station == unit.state.type_id && menu.faction == owner.player.faction_id;
                });
            });
            if (shared == start_->units.end()) shared = std::find_if(start_->units.begin(), start_->units.end(), allied);
            if (shared != start_->units.end()) anchor = *shared;
        }
        // The anchor supplies a composition pose (origin for an empty start). Its entity
        // can belong to a teammate; ownership and colour always belong to this player.
        anchor.state.owner = owner.player.player_id;
        anchors.emplace(owner.player.player_id, std::move(anchor));
    }
    const auto add_slot = [&](const std::string& model, const battle::StartUnit& near,
                              const std::optional<battle::LobbyColour>& colour,
                              const tactical::TypeId required_station = 0, const bool ability_proxy_owner = false,
                              const battle::ArrivalSource source = battle::ArrivalSource::station_spawn) {
        arrival_slots_.plan.add(near.state.owner, model, 1, ability_proxy_owner
            ? battle::ArrivalSource::ability_object : source, near.state.entity_id);
        add_launch_slot(filesystem, catalog, model, near, colour, required_station, ability_proxy_owner);
    };
    // WBP-29: one reusable replacement slot per initially placed respawnable object.
    for (const auto& unit : start_->units) {
        if (economy_.pads.replacement(unit.state.type_id) == nullptr) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& player) { return player.player.player_id == unit.state.owner; });
        add_slot(unit.type, unit, owner != start_->players.end() ? owner->colour : std::optional<battle::LobbyColour>{});
    }
    // Ordinary rows retain their preload order and count. Compact large-launch
    // contributions warm a bounded pool; live demand realizes the remaining slots.
    const std::size_t warm_launches = options_.warm_launches;
    std::set<std::pair<tactical::PlayerId, std::string>> campaign_recipes;
    for (const auto& demand : arrival_slots_.plan.rows) {
        if (demand.source != battle::ArrivalSource::launch) continue;
        const auto spawner = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const auto& unit) { return unit.state.entity_id == demand.anchor; });
        if (!campaign && spawner == start_->units.end()) continue;
        if (campaign && !campaign_recipes.emplace(demand.owner, demand.type).second) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& player) { return player.player.player_id == demand.owner; });
        auto anchor = spawner != start_->units.end() ? *spawner : anchors.at(demand.owner);
        anchor.state.owner = demand.owner;
        const auto count = campaign ? std::size_t{1} : std::min(demand.simultaneous_maximum, warm_launches);
        for (std::size_t index = 0; index < count; ++index)
            add_launch_slot(filesystem, catalog, demand.type, anchor,
                owner != start_->players.end() ? owner->colour : std::optional<battle::LobbyColour>{});
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
        const auto& anchor = anchors.at(builder.player.player_id);
        std::map<tactical::TypeId, std::size_t> slots;
        for (const auto& menu : economy_.menus) {
            if (menu.faction != builder.player.faction_id || economy_.pads.point(menu.station) == nullptr) continue;
            const auto count = std::count_if(start_->units.begin(), start_->units.end(),
                [&](const battle::StartUnit& unit) { return unit.state.type_id == menu.station; });
            for (const auto& option : menu.options) {
                const auto* child = economy_.pads.child(option.type);
                if (!option.available || child == nullptr) continue;
                slots[child->type] += static_cast<std::size_t>(count);
                slots[child->constructed] += static_cast<std::size_t>(count);
            }
        }
        for (const auto& [type, count] : slots) {
            const auto named = std::find_if(tables_->units.begin(), tables_->units.end(),
                [&](const units::UnitType& unit) { return battle::type_id(unit.id) == type; });
            if (named == tables_->units.end()) continue;
            for (std::size_t index = 0; index < count; ++index) add_slot(named->id, anchor, builder.colour);
        }
    }
    for (const tactical::EconomyPlayer& buyer : economy_.players) {
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const battle::StartPlayer& player) { return player.player.player_id == buyer.player; });
        if (owner == start_->players.end()) continue;
        const auto& anchor = anchors.at(buyer.player);
        std::vector<tactical::TypeId> seen;
        std::vector<tactical::TypeId> station_slots;
        for (const tactical::StationMenu& menu : economy_.menus) {
            if (menu.faction != owner->player.faction_id) continue;
            // WPR-52: station replacements use the same preloaded-slot path as arrivals.
            if (menu.next_level != 0 && std::find(station_slots.begin(), station_slots.end(), menu.next_level) == station_slots.end()) {
                station_slots.push_back(menu.next_level);
                const auto next = std::find_if(tables_->units.begin(), tables_->units.end(),
                    [&](const units::UnitType& type) { return battle::type_id(type.id) == menu.next_level; });
                if (next != tables_->units.end()) {
                    add_slot(next->id, anchor, owner->colour);
                    // WPR-52/SK-23: the replacement's larger hangar can launch craft absent
                    // from the initial station. Preload its authored simultaneous complement.
                    if (next->spawner) for (const auto& entry : next->spawner->starting) {
                        const auto* squadron = tables_->find(entry.squadron);
                        if (squadron == nullptr || entry.count <= 0
                            || (!(setup_ && setup_->campaign) && entry.count > 64)) continue;
                        for (std::int32_t index = 0; index < entry.count; ++index) {
                            for (const auto& member : squadron->members) add_slot(member.craft, anchor, owner->colour, menu.next_level);
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
                    [&](const units::UnitType& type) { return battle::type_id(type.id) == deployed; });
                if (named == tables_->units.end()) continue;
                // WR-12/13: separate preloaded visual clones, never launch slots or live entities.
                if (buyer.player == player_) {
                    const auto add_preview = [&](const std::string& craft, const sim::math::Vec3 offset) {
                        BattleScene::PlacedUnit preview;
                        preview.object_id = craft;
                        preview.live_entity = std::numeric_limits<sim::EntityId>::max() / 8U + placement_clones_.size();
                        preview.placement_preview = true;
                        preview.launch_slot = true; // starts hidden until an explicit preview pose
                        preview.team_colour = model_colour(owner->player.player_id, craft);
                        const auto* footprint = economy_.footprint_of(battle::type_id(craft));
                        placement_clones_.push_back({option.type, placed_ships_.size(), offset, footprint ? footprint->layer_z : sim::math::Fixed{}});
                        placed_ships_.push_back(std::move(preview));
                    };
                    if (named->members.empty()) add_preview(named->id, {});
                    for (const auto& member : named->members) add_preview(member.craft, member.offset.value_or(sim::math::Vec3{}));
                }
                const std::uint32_t fits = option.population == 0 ? purchase_slots_per_type : buyer.population_cap / option.population;
                for (std::uint32_t index = 0; index < std::min(fits, purchase_slots_per_type); ++index) {
                    if (named->members.empty()) add_slot(named->id, anchor, owner->colour, 0, false, battle::ArrivalSource::purchase);
                    for (const units::SquadronMember& member : named->members)
                        add_slot(member.craft, anchor, owner->colour, 0, false, battle::ArrivalSource::purchase);
                }
            }
        }
    }
    // Compose one recipe per reserve type first. After initial GPU composition,
    // warm_campaign_slots admits the remaining hull/clone pairs at the load gate.
    if (setup_ && setup_->campaign) {
        std::set<std::pair<tactical::PlayerId, std::string>> warmed;
        for (const auto& demand : arrival_slots_.plan.rows) {
            if (demand.source != battle::ArrivalSource::reinforcement_pool
                && demand.source != battle::ArrivalSource::station_spawn) continue;
            if (demand.source == battle::ArrivalSource::reinforcement_pool
                && !warmed.emplace(demand.owner, demand.type).second) continue;
            battle::StartUnit anchor;
            const auto near = std::find_if(start_->units.begin(), start_->units.end(),
                [&](const auto& unit) { return unit.state.entity_id == demand.anchor; });
            if (near != start_->units.end()) anchor = *near;
            anchor.state.owner = demand.owner;
            const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
                [&](const auto& player) { return player.player.player_id == demand.owner; });
            const auto count = demand.source == battle::ArrivalSource::reinforcement_pool
                ? std::size_t{1} : demand.simultaneous_maximum;
            for (std::size_t index = 0; index < count; ++index)
                add_launch_slot(filesystem, catalog, demand.type, anchor,
                    owner != start_->players.end() ? owner->colour : std::optional<battle::LobbyColour>{});
        }
    }
    // WAB-71/75: ability-created units already reach the snapshot. Reserve reusable visual
    // owners too, including proxy-only models, so their ordinary fog/pose/emitter path runs.
    std::set<std::string> ability_objects;
    for (const auto& type : tables_->units) {
        for (const auto* buzz : {&type.buzz_droids, &type.team_buzz_droids}) {
            if (*buzz && !(*buzz)->object_type.empty()) ability_objects.insert((*buzz)->object_type);
        }
    }
    for (const auto& owner : start_->players) {
        const auto& anchor = anchors.at(owner.player.player_id);
        for (const auto& object : ability_objects) {
            std::size_t holders = 0;
            const auto holds = [&](const std::string& id) {
                const auto* type = tables_->find(id);
                if (!type) return false;
                return (type->buzz_droids && type->buzz_droids->object_type == object)
                    || (type->team_buzz_droids && type->team_buzz_droids->object_type == object);
            };
            for (const auto& unit : start_->units) {
                if (unit.state.owner == owner.player.player_id && holds(unit.type)
                    && economy_.pads.replacement(unit.state.type_id) == nullptr) ++holders;
            }
            // Include purchased and hangar-spawned holders whose slots were prepared above.
            const auto existing_slots = launch_slots_.size();
            for (std::size_t index = 0; index < existing_slots; ++index) {
                const auto& slot = launch_slots_[index];
                if (slot.station_owner == owner.player.player_id && holds(placed_ships_[slot.ship].object_id)) ++holders;
            }
            for (std::size_t index = 0; index < holders; ++index) add_slot(object, anchor, owner.colour, 0, true);
        }
    }
    // #458: each slot's death clone, set up like a start unit's (the clones used to be planned
    // for the start units only, so a craft launched after tick zero died without one). The
    // clone's clip variant is drawn from the slot's own entity, as the craft's ID is not known
    // until it launches.
    arrival_slots_.purchases.clear();
    for (const auto& demand : arrival_slots_.plan.rows) if (demand.source == battle::ArrivalSource::purchase)
        arrival_slots_.purchases.emplace(demand.owner, battle::type_id(demand.type));
    for (LaunchSlot& slot : launch_slots_) {
        if (auto clone = prepare_death_clone(filesystem, catalog, slot.ship, placed_ships_[slot.ship].live_entity)) {
            slot.clone = launch_clones_.size();
            launch_clones_.push_back(std::move(*clone));
        }
    }
}

bool LiveSessionView::warm_campaign_slots(BattleScene& population, GodotRenderer& renderer) {
    if (options_.land || !(setup_ && setup_->campaign) || !arrival_slots_.filesystem || !arrival_slots_.catalog) return true;
    FrameTimer load_timer(&arrival_slots_.warm_ms);
    using Key = std::pair<tactical::PlayerId, std::string>;
    std::map<Key, ReserveWarmCost> indexed;
    // Placement previews also carry the hidden-art flag launch_slot, but are
    // private ghosts. Only the LaunchSlot roster supplies reusable warm capacity.
    for (const auto& slot : launch_slots_) {
        const Key key{slot.station_owner, placed_ships_[slot.ship].object_id};
        auto [found, inserted] = indexed.try_emplace(key);
        auto& cost = found->second;
        ++cost.existing;
        if (!inserted) continue;
        cost.owner = key.first;
        cost.type = key.second;
        cost.instance_bytes = population.live_copy_bytes(renderer, slot.ship);
        if (slot.clone && cost.instance_bytes) {
            const auto clone = population.live_copy_bytes(renderer, launch_clones_[*slot.clone].ship);
            if (!clone || *clone > std::numeric_limits<std::uint64_t>::max() - *cost.instance_bytes)
                cost.instance_bytes.reset();
            else *cost.instance_bytes += *clone;
        }
    }
    std::vector<ReserveWarmCost> costs;
    for (const auto& [key, cost] : indexed) { static_cast<void>(key); costs.push_back(cost); }
    // This is a loading-only probe. No RenderingServer getter is added to a frame.
    const auto memory = GodotRenderer::gpu_memory();
    const auto device = memory.budget.device_bytes;
    const auto limit = usable_device_budget_bytes(device);
    const auto accounted = memory.budget.other_bytes > std::numeric_limits<std::uint64_t>::max() - memory.budget.resident_bytes
        ? std::numeric_limits<std::uint64_t>::max() : memory.budget.other_bytes + memory.budget.resident_bytes;
    const auto used = std::max(memory.device_total, accounted);
    const bool budget_off = memory.budget.source == "off";
    const bool probe_available = memory.budget.constrained && device != 0;
    // Only an explicit measurement switch disables admission limits. An unknown
    // probe must never turn the entire authored campaign roster into an allocation.
    if (!budget_off && !probe_available) for (auto& cost : costs) cost.instance_bytes.reset();
    const ReserveWarmBudget budget{!budget_off, limit > used ? limit - used : 0};
    const auto plan = plan_reserve_warm(arrival_slots_.plan.rows, costs, budget, true);
    arrival_slots_.warm_complete = arrival_slots_.warm_complete && plan.complete();
    arrival_slots_.warm_probe_available = probe_available;
    arrival_slots_.warm_budget_off = budget_off;
    arrival_slots_.warm_available_bytes = budget.available_bytes;
    arrival_slots_.warm_estimated_bytes = plan.additional_bytes;
    if (!plan.valid) return true; // Retain the finite recipe prefix and existing fallback.
    const auto first_ship = placed_ships_.size();
    const auto& filesystem = *arrival_slots_.filesystem;
    const auto& catalog = *arrival_slots_.catalog;
    for (const auto& row : plan.rows) {
        arrival_slots_.warm_requested += row.requested;
        arrival_slots_.warm_admitted += row.admitted;
        const auto existing = indexed.find({row.owner, row.type});
        if (existing == indexed.end()) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& player) { return player.player.player_id == row.owner; });
        battle::StartUnit anchor;
        anchor.state.owner = row.owner;
        for (std::size_t count = existing->second.existing; count < row.admitted; ++count) {
            const auto ship = placed_ships_.size();
            add_launch_slot(filesystem, catalog, row.type, anchor,
                owner != start_->players.end() ? owner->colour : std::optional<battle::LobbyColour>{});
            if (auto clone = prepare_death_clone(filesystem, catalog, ship, placed_ships_[ship].live_entity)) {
                launch_slots_.back().clone = launch_clones_.size();
                launch_clones_.push_back(std::move(*clone));
            }
        }
    }
    // S-foil art belongs to loading too, just as for the original launch slots.
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    for (auto& ship : std::span(placed_ships_).subspan(first_ship)) {
        const auto* model = tables_->find(ship.object_id);
        if (!model) continue;
        ship.defend_shell = std::any_of(model->abilities.begin(), model->abilities.end(),
            [](const auto& row) { return row.type == "DEFEND"; });
        ship.stealth_shell = std::any_of(model->abilities.begin(), model->abilities.end(),
            [](const auto& row) { return row.type == "STEALTH"; });
        if (!ship.clip.empty() || std::none_of(model->abilities.begin(), model->abilities.end(),
            [](const auto& row) { return row.type == "SPOILER_LOCK"; })) continue;
        const auto object = catalog.resolve(ship.object_id, data::Category::game_object);
        if (!object) continue;
        const auto path = space_model_path(object.value());
        const auto override_name = tag_text(object.value(), "Space_Model_Anim_Override_Name");
        const auto deploy = animation::model_clip_paths(path, animation::clip_types::deploy, exists, override_name);
        const auto undeploy = animation::model_clip_paths(path, animation::clip_types::undeploy, exists, override_name);
        if (!deploy.empty() && !undeploy.empty()) { ship.clip = deploy.front(); ship.alternate_clip = undeploy.front(); }
    }
    return population.append_live_ships(renderer, filesystem, std::span(placed_ships_).subspan(first_ship));
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
        // The physical slot returns now, also for a terminally retired source. The shadow pool keeps that
        // slot reserved for the session (the seed identity, #4218); every other unit frees both at once. The
        // identity entry stays (not held): a spin-away craft leaves the snapshot and rebinds every frame, and
        // the rebind then updates the entry in place, with no node allocation.
        for (LaunchSlot& slot : launch_slots_) {
            if (slot.ship == bound->second) slot.bound = false;
        }
        if (!terminal_source_leases_.contains(bound->first)) {
            if (const auto identity = slot_identity_of_entity_.find(bound->first);
                identity != slot_identity_of_entity_.end() && identity->second.held) {
                for (LaunchSlot& slot : launch_slots_) {
                    if (slot.ship == identity->second.ship) slot.shadow_bound = false;
                }
                --shadow_held_;
                identity->second.held = false;
            }
        }
        death_clones_.erase(bound->first);
        ++slots_released_;
        bound = launched_ship_of_entity_.erase(bound);
    }
}

// #4218: pick the slot this unit binds and the slot its particle seeds and idle start come from. The shadow pool
// allocates exactly as the reservation policy did (the first free slot of the type and owner, terminal sources held
// for the session), so a real slot reused early still draws the seeds of the slot that policy would have bound.
// What is drawn follows the shadow pick, which is the slot BASE binds: when the shadow and the real slot differ in
// drawn state, the unit binds the shadow slot itself (no early reuse for it, counted in identity_undrawn_). Two
// undrawn slots draw nothing either way (counted in identity_both_undrawn_). When the shadow pool has no free slot,
// or the shadow slot is still bound, the real slot stands in (counted in identity_fallbacks_); none of this
// happens without early reuse. The returned slot is the one to bind. The map keeps one integer entry per launched
// entity for the session; a unit that rebinds updates its entry in place.
LiveSessionView::LaunchSlot& LiveSessionView::bind_slot_identity(const sim::EntityId entity, LaunchSlot& real,
    const tactical::TypeId type, const tactical::PlayerId owner) {
    const auto drawn = [&](const std::size_t ship) {
        return !arrival_slots_.population || arrival_slots_.population->live_ship_drawn(ship);
    };
    auto known = slot_identity_of_entity_.find(entity);
    if (known != slot_identity_of_entity_.end() && known->second.held) {
        // A terminal source keeps its reserved shadow slot; it never takes a second one.
        if (arrival_slots_.population) arrival_slots_.population->bind_live_identity(real.ship, known->second.ship);
        return real;
    }
    LaunchSlot* shadow = nullptr;
    for (LaunchSlot& slot : launch_slots_) {
        if (slot.shadow_bound || slot.type != type || slot.station_owner != owner || slot.required_station != 0) continue;
        shadow = &slot;
        break;
    }
    LaunchSlot* home = &real;
    live_session::SlotIdentity identity{real.ship, false};
    if (shadow) {
        const bool shadow_drawn = drawn(shadow->ship), mismatch = shadow_drawn != drawn(real.ship);
        if (mismatch && (real.bound || shadow->bound)) {
            ++identity_fallbacks_;  // a taken slot cannot be swapped for the shadow slot
        } else {
            if (mismatch) {
                home = shadow;
                ++identity_undrawn_;
            } else if (!shadow_drawn) {
                ++identity_both_undrawn_;
            }
            shadow->shadow_bound = true;
            identity = {shadow->ship, true};
            ++shadow_held_;
        }
    } else {
        ++identity_fallbacks_;
    }
    slot_identity_of_entity_[entity] = identity;  // in place when the unit rebinds
    shadow_peak_ = std::max(shadow_peak_, shadow_held_);
    if (arrival_slots_.population) arrival_slots_.population->bind_live_identity(home->ship, identity.ship);
    return *home;
}

std::optional<std::size_t> LiveSessionView::ship_of(const sim::EntityId entity, const tactical::TypeId type,
                                                const tactical::PlayerId owner) {
    const auto drawn = [&](const std::size_t ship) -> std::optional<std::size_t> {
        const auto missing = std::lower_bound(arrival_slots_.undrawn.begin(), arrival_slots_.undrawn.end(), entity);
        if (missing != arrival_slots_.undrawn.end() && *missing == entity) arrival_slots_.undrawn.erase(missing);
        if (const auto missed = slotless_hits_.find({owner, type}); missed != slotless_hits_.end()) {
            missed->second.erase(entity);
            if (missed->second.empty()) slotless_hits_.erase(missed);
        }
        return ship;
    };
    if (const auto found = ship_of_entity_.find(entity); found != ship_of_entity_.end()) return drawn(found->second);
    if (const auto found = launched_ship_of_entity_.find(entity); found != launched_ship_of_entity_.end()) return drawn(found->second);
    for (LaunchSlot& candidate : launch_slots_) {
        // The model and its death clone were composed with this owner's lobby colour.
        if (candidate.bound || candidate.type != type || candidate.station_owner != owner || candidate.required_station != 0) continue;
        LaunchSlot& slot = bind_slot_identity(entity, candidate, type, owner);  // the candidate, or the shadow slot
        if (slot.clone) {
            if (!arrival_slots_.population || !arrival_slots_.renderer) {
                arrival_slots_.failure = "cannot bind a clone slot outside frame composition";
                return std::nullopt;
            }
            arrival_slots_.population->restore_live_ship(*arrival_slots_.renderer, launch_clones_[*slot.clone].ship);
        }
        slot.bound = true;
        launched_ship_of_entity_.emplace(entity, slot.ship);
        real_peak_ = std::max(real_peak_, launched_ship_of_entity_.size());
        if (slot.clone) death_clones_.emplace(entity, launch_clones_[*slot.clone]);
        return drawn(slot.ship);
    }
    if (const auto ship = grow_launch_slot(entity, type, owner)) {
        auto& slot = launch_slots_.back();
        slot.bound = true;
        launched_ship_of_entity_.emplace(entity, *ship);
        bind_slot_identity(entity, slot, type, owner);
        real_peak_ = std::max(real_peak_, launched_ship_of_entity_.size());
        if (slot.clone) death_clones_.emplace(entity, launch_clones_[*slot.clone]);
        return drawn(*ship);
    }
    const auto named = presentation_types_.find(type);
    const bool container = named != presentation_types_.end()
        && named->second->kind == units::UnitKind::squadron && !named->second->members.empty();
    if (!container) {
        const auto missing = std::lower_bound(arrival_slots_.undrawn.begin(), arrival_slots_.undrawn.end(), entity);
        if (missing == arrival_slots_.undrawn.end() || *missing != entity) arrival_slots_.undrawn.insert(missing, entity);
    }
    // #3957: never silent. A live unit with no slot is simulated but not drawn; note each such entity
    // once by owner and type (allocates on its first sighting only, not per call or frame) for the report.
    slotless_hits_[{owner, type}].insert(entity);
    return std::nullopt;
}

std::optional<std::size_t> LiveSessionView::grow_launch_slot(const sim::EntityId entity,
    const tactical::TypeId type, const tactical::PlayerId owner) {
    // Planned launches may outgrow the warm pool. Purchase provenance remains
    // necessary for other skirmish types; a shared menu entry alone is insufficient.
    const bool launch = std::any_of(arrival_slots_.plan.rows.begin(), arrival_slots_.plan.rows.end(),
        [&](const auto& row) { return row.source == battle::ArrivalSource::launch
            && row.owner == owner && battle::type_id(row.type) == type; });
    // A launched craft must be drawn in the frame it launches: planned launches are exempt from the per-frame
    // arrival budget (their count is bounded by the plan's simultaneous maximum; warm_launches sets how many are
    // composed at load). Only purchases and reinforcement pools wait for the budget.
    if (!arrival_slots_.population || !arrival_slots_.renderer || !arrival_slots_.filesystem || !arrival_slots_.catalog
        || (!launch && !arrival_slots_.remaining) || arrival_slots_.failed.contains({owner, type}) || !tables_ || !start_)
        return std::nullopt;
    if (!(setup_ && setup_->campaign)
        && !launch && (!arrival_slots_.purchases.contains({owner, type}) || !arrivals_.contains(entity))) return std::nullopt;
    const units::UnitType* named = nullptr;
    if (const auto indexed = presentation_types_.find(type); indexed != presentation_types_.end()) named = indexed->second;
    else {
        const auto found = std::find_if(tables_->units.begin(), tables_->units.end(),
            [&](const auto& row) { return battle::type_id(row.id) == type; });
        if (found != tables_->units.end()) named = &*found;
    }
    if (named == nullptr || (named->kind == units::UnitKind::squadron && !named->members.empty()))
        return std::nullopt;
    FrameTimer growth_timer(trace_frames_ ? &arrival_slots_.growth_ms : nullptr, true);
    if (!launch) --arrival_slots_.remaining; // deterministic work budget, independent of elapsed frame time
    // Projectile pools append model slots independently. Keep this metadata indexed by
    // the population's actual slots before adding a hull and its death clone.
    const auto composed = arrival_slots_.population->placed_units();
    if (composed.size() > placed_ships_.size()) {
        const auto added = composed.subspan(placed_ships_.size());
        placed_ships_.insert(placed_ships_.end(), added.begin(), added.end());
    }
    const auto first_ship = placed_ships_.size();
    battle::StartUnit anchor;
    // The origin is only a composition anchor: appended pieces stay hidden until posed.
    anchor.state.owner = owner;
    const auto colour = std::find_if(start_->players.begin(), start_->players.end(),
        [&](const auto& row) { return row.player.player_id == owner; });
    bool proxy_owner = false;
    for (const auto& demand : arrival_slots_.plan.rows) if (demand.type == named->id
        && demand.source == battle::ArrivalSource::ability_object) { proxy_owner = true; break; }
    if (!proxy_owner) for (const auto& holder : tables_->units) {
        if ((holder.buzz_droids && holder.buzz_droids->object_type == named->id)
            || (holder.team_buzz_droids && holder.team_buzz_droids->object_type == named->id)) {
            proxy_owner = true;
            break;
        }
    }
    add_launch_slot(*arrival_slots_.filesystem, *arrival_slots_.catalog, named->id, anchor,
        colour != start_->players.end() ? colour->colour : std::optional<battle::LobbyColour>{}, 0, proxy_owner);
    const auto slot_index = launch_slots_.size() - 1;
    if (auto clone = prepare_death_clone(*arrival_slots_.filesystem, *arrival_slots_.catalog, first_ship, entity)) {
        launch_slots_[slot_index].clone = launch_clones_.size();
        launch_clones_.push_back(std::move(*clone));
    }
    const auto exists = [&](const std::string& path) { return static_cast<bool>(arrival_slots_.filesystem->stat(path)); };
    for (auto& ship : std::span(placed_ships_).subspan(first_ship)) {
        const auto* model = tables_->find(ship.object_id);
        if (!model) continue;
        const auto has = [&](const std::string_view ability) {
            return std::any_of(model->abilities.begin(), model->abilities.end(),
                [&](const auto& row) { return row.type == ability; });
        };
        ship.defend_shell = has("DEFEND");
        ship.stealth_shell = has("STEALTH");
        if (!ship.clip.empty() || !has("SPOILER_LOCK")) continue;
        const auto object = arrival_slots_.catalog->resolve(ship.object_id, data::Category::game_object);
        if (!object) continue;
        const auto path = space_model_path(object.value());
        const auto override_name = tag_text(object.value(), "Space_Model_Anim_Override_Name");
        const auto deploy = animation::model_clip_paths(path, animation::clip_types::deploy, exists, override_name);
        const auto undeploy = animation::model_clip_paths(path, animation::clip_types::undeploy, exists, override_name);
        if (!deploy.empty() && !undeploy.empty()) { ship.clip = deploy.front(); ship.alternate_clip = undeploy.front(); }
    }
    if (!arrival_slots_.population->append_live_ships(*arrival_slots_.renderer, *arrival_slots_.filesystem,
            std::span(placed_ships_).subspan(first_ship)) || !arrival_slots_.population->live_ship_drawn(first_ship)) {
        // An undrawable authored type is reported once; retrying it each frame would allocate forever.
        launch_slots_[slot_index].bound = true;
        launch_slots_[slot_index].shadow_bound = true;
        arrival_slots_.failed.emplace(owner, type);
        arrival_slots_.failure = arrival_slots_.population->failure();
        if (arrival_slots_.failure.empty()) arrival_slots_.failure = "arrival type has no drawable model: " + named->id;
        return std::nullopt;
    }
    ++arrival_slots_.grown;
    ++arrival_slots_.frame_grown;
    return first_ship;
}

const tactical::Squadron* LiveSessionView::squadron_of(const sim::EntityId craft) const noexcept {
    const auto container = squadron_of_.find(craft);
    if (container == squadron_of_.end()) return nullptr;
    const auto squadron = squadrons_.find(container->second);
    return squadron == squadrons_.end() ? nullptr : &squadron->second;
}

} // namespace eawr::presentation::godot_backend
