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

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::player_colour(const tactical::PlayerId player) const {
    if (!start_) return std::nullopt;
    for (const skirmish::StartPlayer& entry : start_->players) {
        if (entry.player.player_id == player && entry.colour) return entry.colour->rgb;
    }
    return std::nullopt;
}

bool LiveSessionView::is_ally_of_local(const tactical::PlayerId owner) const {
    if (owner == player_) return true;
    const auto own = team_of_player_.find(player_);
    const auto other = team_of_player_.find(owner);
    return own != team_of_player_.end() && other != team_of_player_.end() && own->second == other->second;
}

const tactical::PadView* LiveSessionView::pad_view(const sim::EntityId entity) const noexcept {
    if (!battle_frame_.latest) return nullptr;
    const auto views = battle_frame_.latest->pads();
    const auto found = std::lower_bound(views.begin(), views.end(), entity,
        [](const auto& view, const auto id) { return view.entity < id; });
    return found != views.end() && found->entity == entity ? &*found : nullptr;
}

bool LiveSessionView::pad_visible(const sim::EntityId entity) const noexcept {
    const auto* pad = pad_view(entity);
    if (pad == nullptr) return true;
    const auto* instance = snapshot_index_.instance(entity);
    if (instance == nullptr || !tables_) return false;
    const auto named = presentation_types_.find(instance->type_id);
    if (named == presentation_types_.end()) return false;
    // WBP-01/35: empty-pad visibility gates build pads, not ordinary capturable structures.
    const auto* profile = economy_.pads.point(instance->type_id);
    if (profile != nullptr && !profile->build_pad) return true;
    // WBP-35: object visibility and fog remain separate gates.
    if (pad->state.under_construction || pad->state.constructed) return !named->second->hides_when_built_on;
    return named->second->visible_to_enemies_when_empty || is_ally_of_local(instance->owner);
}

bool LiveSessionView::pad_action_allowed(const sim::EntityId entity) const {
    const auto* pad = pad_view(entity);
    const auto* instance = snapshot_index_.instance(entity);
    const auto faction = local_faction_id();
    if (pad == nullptr || instance == nullptr || !faction || !setup_ || !pad_visible(entity)
        || !is_ally_of_local(instance->owner)
        || (!options_.reveal && !std::binary_search(snapshot_index_.visible().begin(), snapshot_index_.visible().end(), entity))) return false;
    const auto* profile = economy_.pads.point(instance->type_id);
    const auto* menu = economy_.menu(instance->type_id, *faction);
    if (profile == nullptr || !profile->build_pad || menu == nullptr || menu->options.empty()) return false;
    // WBP-08: proximity is evaluated only for the opening action, never for a palette refresh.
    std::vector<tactical::CaptureCandidate> candidates;
    candidates.reserve(battle_frame_.latest->instances().size());
    for (const auto& candidate : battle_frame_.latest->instances()) {
        candidates.push_back({{candidate.entity_id, candidate.owner,
            {candidate.fixed_transform.rows[0][3], candidate.fixed_transform.rows[1][3], candidate.fixed_transform.rows[2][3]}},
            candidate.type_id, !candidate.arrival.has_value()});
    }
    const sim::math::Vec3 position{instance->fixed_transform.rows[0][3], instance->fixed_transform.rows[1][3], instance->fixed_transform.rows[2][3]};
    return tactical::pad_construction_allowed(*profile, pad->state, player_, setup_->players, candidates,
        economy_.pads, entity, position);
}

sim::EntityId LiveSessionView::pad_selection_target(const sim::EntityId entity) const noexcept {
    if (const auto found = construction_successors_.find(entity); found != construction_successors_.end()) return found->second;
    const auto* pad = pad_view(entity);
    return pad ? ui::pad_pick_target(entity, !pad_visible(entity), pad->state.under_construction, pad->state.constructed) : entity;
}

const tactical::ConstructionState* LiveSessionView::pad_construction(const sim::EntityId child) const noexcept {
    const auto found = presented_construction_.find(child);
    return found == presented_construction_.end() ? nullptr : &found->second;
}

std::optional<ui::UnitAbilityState> LiveSessionView::Abilities::state(const sim::EntityId unit,
                                                                     const std::uint32_t ability) const {
    const tactical::AbilityKind kind = tactical::ability_kind(ui::ability_name(ability));
    // AB-03: a cut ability the unit's type authors keeps its button, never usable.
    if (kind == tactical::AbilityKind::none) return ui::UnitAbilityState{ui::AbilityStatus::disabled, 1.0, false};
    const auto& snapshot = view_.ability_snapshot_;
    if (!snapshot) return std::nullopt;
    const std::array<sim::EntityId, 1> single{unit};
    std::span<const sim::EntityId> holders = single;
    // A squadron's container stands for its craft (AB-15), except for a team ability, which the
    // container holds itself (#561, AB-60).
    const auto squadron = view_.squadron_members_.find(unit);
    if (squadron != view_.squadron_members_.end() && kind != tactical::AbilityKind::ion_cannon_shot) {
        holders = squadron->second;
    }
    bool has = false;
    bool all_active = true;
    bool all_autofire = true;
    bool blocked = false;
    std::optional<double> recharge;
    std::optional<double> active_dial; // AB-05: the largest share of a timed duration left among the units that are on
    // WU-46: the cards and world overlays share this lookup. Visit only this unit's holders;
    // sorted snapshot lookup neither allocates nor scans unrelated units on each drawn frame.
    for (const sim::EntityId holder : holders) {
        const auto* instance = space::find_instance(*snapshot, holder);
        if (instance == nullptr) continue;
        for (const tactical::AbilityStatus& status : instance->abilities) {
            if (status.kind != kind) continue;
            has = true;
            all_active = all_active && status.active;
            all_autofire = all_autofire && status.autofire;
            if (status.active && status.total_frames > 0) {
                const double left = static_cast<double>(status.remaining_frames) / status.total_frames;
                active_dial = active_dial ? std::max(*active_dial, left) : left;
            }
            if (!status.active && !status.ready) {
                if (status.remaining_frames > 0 && status.total_frames > 0) {
                    const double done = 1.0 - static_cast<double>(status.remaining_frames) / status.total_frames;
                    recharge = recharge ? std::min(*recharge, done) : done;
                } else {
                    blocked = true; // AB-14, AB-16: its gate holds it
                }
            }
        }
    }
    if (!has) return ui::UnitAbilityState{ui::AbilityStatus::disabled, 1.0, false};
    ui::UnitAbilityState result;
    result.autofire = all_autofire;
    if (all_active) {
        result.status = ui::AbilityStatus::active;
        if (active_dial) result.recharge = std::clamp(*active_dial, 0.0, 1.0);
    } else if (recharge) {
        result.status = ui::AbilityStatus::recharging;
        result.recharge = std::clamp(*recharge, 0.0, 1.0);
    } else if (blocked) {
        result.status = ui::AbilityStatus::disabled;
    }
    return result;
}

void LiveSessionView::Abilities::request(const ui::AbilityRequest& request) {
    const tactical::AbilityKind kind = tactical::ability_kind(ui::ability_name(request.ability));
    // #561: a targeted activation goes out once it has its target (the battle input's targeting).
    const bool aimed = request.targeted && (request.target != sim::invalid_entity_id || request.position.has_value());
    if (kind == tactical::AbilityKind::none || (request.targeted && !aimed) || !view_.scheduler_) {
        ++refused_;
        return;
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::ability;
    intent.units = request.units;
    intent.unit_ability = kind;
    intent.target = aimed ? request.target : sim::invalid_entity_id;
    if (request.position) intent.destination = *request.position;
    intent.origin = aimed ? ui::CommandOrigin::world_click : ui::CommandOrigin::hud_button;
    switch (request.kind) {
    case ui::AbilityRequest::Kind::activate: intent.ability_action = tactical::AbilityAction::activate; break;
    case ui::AbilityRequest::Kind::deactivate: intent.ability_action = tactical::AbilityAction::deactivate; break;
    case ui::AbilityRequest::Kind::autofire_on: intent.ability_action = tactical::AbilityAction::autofire_on; break;
    case ui::AbilityRequest::Kind::autofire_off: intent.ability_action = tactical::AbilityAction::autofire_off; break;
    }
    // The simulation decides which units act; a unit that cannot is rejected alone (AB-11 to AB-15).
    if (view_.scheduler_->issue(intent)) {
        ++issued_;
        if (intent.ability_action == tactical::AbilityAction::activate
            || intent.ability_action == tactical::AbilityAction::deactivate) {
            view_.ability_clicks_.push_back({kind, intent.ability_action == tactical::AbilityAction::activate, request.units});
        }
    } else {
        ++refused_;
    }
}

const tactical::EconomyView* LiveSessionView::local_economy() const noexcept {
    if (!battle_frame_.latest) return nullptr;
    for (const tactical::EconomyView& view : battle_frame_.latest->economy()) {
        if (view.player == player_) return &view;
    }
    return nullptr;
}

bool LiveSessionView::build_allowed(const tactical::BuildOption& option) const {
    return build_menu_state(option).enabled;
}

ui::BuildOptionState LiveSessionView::build_menu_state(const tactical::BuildOption& option) const {
    if (session_) {
        return ui::build_option_state(option, [&](const tactical::TypeId type) {
            return session_->production_counts(player_, type);
        });
    }
    const auto snapshot = battle_frame_.latest;
    if (!snapshot) return {false, false};
    return ui::build_option_state(option, [&](const tactical::TypeId type) {
        tactical::ProductionCounts counts;
        for (const auto& player : snapshot->players()) {
            if (!is_ally_of_local(player.player_id)) continue;
            const auto owner = player.player_id;
            auto owned = static_cast<std::uint64_t>(std::count_if(snapshot->instances().begin(), snapshot->instances().end(),
                [&](const tactical::TacticalInstance& unit) { return unit.owner == owner && unit.type_id == type; }));
            std::uint64_t current = owned, lifetime = 0;
            for (const auto& ledger : snapshot->economy()) {
                if (ledger.player != owner) continue;
                owned += static_cast<std::uint64_t>(std::count_if(ledger.completed.begin(), ledger.completed.end(),
                    [&](const tactical::CompletedBuild& object) { return object.type == type; }));
                current = owned + static_cast<std::uint64_t>(std::count(ledger.pool.begin(), ledger.pool.end(), type));
                for (const auto& queue : ledger.queues) {
                    const auto queued = static_cast<std::uint64_t>(std::count_if(queue.begin(), queue.end(),
                        [&](const tactical::QueueEntry& entry) { return entry.type == type; }));
                    current += queued;
                    counts.queued_allies += queued;
                    if (owner == player_) counts.queued_player += queued;
                }
                if (const auto built = ledger.lifetime.find(type); built != ledger.lifetime.end()) lifetime = built->second;
            }
            counts.current_allies += current; counts.lifetime_allies += lifetime;
            if (owner == player_) {
                counts.owned_player = owned; counts.current_player = current; counts.lifetime_player = lifetime;
            }
        }
        return counts;
    });
}

std::optional<tactical::FactionId> LiveSessionView::local_faction_id() const noexcept {
    if (!setup_) return std::nullopt;
    for (const tactical::Player& player : setup_->players) {
        if (player.player_id == player_) return player.faction_id;
    }
    return std::nullopt;
}

namespace {

// #530: an economy intent through the order scheduler; false when the scheduler refused it.
bool issue_economy(ui::CommandScheduler* scheduler, const ui::TacticalIntent& intent) {
    return scheduler != nullptr && static_cast<bool>(scheduler->issue(intent));
}

} // namespace

bool LiveSessionView::pad_sale_allowed(const sim::EntityId child, const bool single_step) const {
    const auto* instance = snapshot_index_.instance(child);
    if (instance == nullptr || !battle_frame_.latest || local_economy() == nullptr) return false;
    const auto parents = battle_frame_.latest->pads();
    const auto parent = std::find_if(parents.begin(), parents.end(),
        [child](const auto& pad) { return pad.state.constructed == child; });
    return tactical::pad_sale_permission(player_, instance->owner, economy_.pad_sale(instance->type_id) != nullptr,
        parent != parents.end() ? &parent->state : nullptr, single_step);
}

bool LiveSessionView::sell_pad_structure(const sim::EntityId child, const bool single_step) {
    if (!pad_sale_allowed(child, single_step)) return false;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::pad_sell;
    intent.units = {child};
    intent.origin = ui::CommandOrigin::world_click;
    return issue_economy(scheduler_.get(), intent);
}

bool LiveSessionView::buy(const sim::EntityId station, const tactical::TypeId type) {
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::buy;
    if (const auto latest = battle_frame_.latest) {
        const auto pad = std::find_if(latest->pads().begin(), latest->pads().end(),
            [station](const tactical::PadView& view) { return view.entity == station; });
        if (pad != latest->pads().end()) {
            const auto* instance = snapshot_index_.instance(station);
            const auto* profile = instance != nullptr ? economy_.pads.point(instance->type_id) : nullptr;
            // WBP-33: capturable producers use the ordinary queue, not pad construction.
            if (profile != nullptr && profile->build_pad) intent.verb = ui::TacticalVerb::pad_build;
        }
    }
    intent.units = {station};
    intent.type = type;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.buys : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::cancel_build(const tactical::BuildQueue queue, const std::uint32_t index) {
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::cancel;
    intent.queue = queue;
    intent.index = index;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.cancels : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::reinforce(const tactical::TypeId type, const sim::math::Vec3& point) {
    // WR-15: drop checks placement before room. A busy preview query has no authority;
    // the cached same-point verdict may be used, and the command independently rechecks state.
    const auto valid = session_ ? session_->reinforcement_point(player_, type, point) : std::optional<bool>{false};
    const bool cached = preview_type_ == type && preview_point_ == point && preview_valid_;
    if (!reinforcement_allowed() || !valid.value_or(cached) || !reinforcement_room(type)) {
        ++economy_requests_.refused;
        return false;
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::reinforce;
    intent.type = type;
    intent.destination = point;
    intent.origin = ui::CommandOrigin::world_click;
    const std::uint64_t tick = order_tick();
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.reinforcements : economy_requests_.refused);
    if (issued) {
        std::erase_if(pending_reinforcements_, [this](const PendingReinforcement& drop) { return !reinforcement_pending(drop); });
        pending_reinforcements_.push_back({tick, type});
    }
    return issued;
}

bool LiveSessionView::reinforcement_pending(const PendingReinforcement& drop) const noexcept {
    // A command stamped for tick t is applied by the step that completes tick t + 1.
    return !battle_frame_.latest || battle_frame_.latest->completed_tick() <= drop.tick;
}

std::uint32_t LiveSessionView::population_of(const tactical::TypeId type) const noexcept {
    for (const auto& menu : economy_.menus) {
        if (const auto* option = menu.find(type)) return option->population;
    }
    return 0U;
}

std::vector<tactical::TypeId> LiveSessionView::reinforcement_pool() const {
    const auto* ledger = local_economy();
    if (ledger == nullptr) return {};
    std::vector<tactical::TypeId> pool = ledger->pool;
    for (const PendingReinforcement& drop : pending_reinforcements_) {
        if (!reinforcement_pending(drop)) continue;
        if (const auto found = std::find(pool.begin(), pool.end(), drop.type); found != pool.end()) pool.erase(found);
    }
    return pool;
}

std::uint32_t LiveSessionView::pending_reinforcement_population() const {
    std::uint32_t population = 0;
    for (const PendingReinforcement& drop : pending_reinforcements_) {
        if (reinforcement_pending(drop)) population += population_of(drop.type);
    }
    return population;
}

std::size_t LiveSessionView::pending_reinforcements() const {
    return static_cast<std::size_t>(std::count_if(pending_reinforcements_.begin(), pending_reinforcements_.end(),
        [this](const PendingReinforcement& drop) { return reinforcement_pending(drop); }));
}

bool LiveSessionView::reinforcement_allowed() const noexcept {
    if (!setup_ || outcome_ || local_economy() == nullptr) return false;
    const auto found = std::find_if(setup_->players.begin(), setup_->players.end(), [this](const tactical::Player& player) {
        return player.player_id == player_;
    });
    return found != setup_->players.end() && (found->flags & tactical::player_flag_commandable) != 0;
}

bool LiveSessionView::reinforcement_room(const tactical::TypeId type) const noexcept {
    const auto* ledger = local_economy();
    if (ledger == nullptr) return false;
    // A unit dropped while paused has left the pool and taken its population (TM-10).
    std::size_t waiting = 0;
    std::uint64_t population = ledger->population;
    for (const PendingReinforcement& drop : pending_reinforcements_) {
        if (!reinforcement_pending(drop)) continue;
        if (drop.type == type) ++waiting;
        population += population_of(drop.type);
    }
    if (static_cast<std::size_t>(std::count(ledger->pool.begin(), ledger->pool.end(), type)) <= waiting) return false;
    const std::uint32_t added = population_of(type);
    return population <= ledger->population_cap && added <= ledger->population_cap - population
        && std::any_of(economy_.menus.begin(), economy_.menus.end(), [type](const auto& menu) { return menu.find(type) != nullptr; });
}

void LiveSessionView::placement_preview(const std::optional<tactical::TypeId> type,
    const std::optional<sim::math::Vec3> point) {
    if (preview_type_ != type || preview_point_ != point) {
        preview_valid_ = false;
        preview_checked_tick_.reset();
    }
    preview_type_ = type;
    preview_point_ = point;
    if (!type || !point || !reinforcement_allowed()) return;
    // WR-13: an unchanged cursor and snapshot need one predicate query, even while paused.
    const auto tick = session_->completed_tick();
    if (preview_checked_tick_ == tick) return;
    ++preview_queries_;
    if (const auto valid = session_->reinforcement_point(player_, *type, *point)) {
        preview_valid_ = *valid;
        preview_checked_tick_ = tick;
    }
}

std::string LiveSessionView::player_faction(const tactical::PlayerId player) const {
    if (start_) {
        for (const skirmish::StartPlayer& candidate : start_->players) {
            if (candidate.player.player_id == player) return candidate.faction;
        }
        return {};
    }
    // Replays retain faction IDs rather than lobby names; all owners need arrival audio.
    if (!setup_) return {};
    for (const auto& candidate : setup_->players) {
        if (candidate.player_id != player) continue;
        for (const std::string_view name : {"Empire", "Rebel", "Underworld"}) {
            if (skirmish::faction_id(name) == candidate.faction_id) return std::string(name);
        }
    }
    return {};
}

std::optional<tactical::TeamId> LiveSessionView::team_of(const tactical::PlayerId player) const {
    const auto found = team_of_player_.find(player);
    if (found == team_of_player_.end()) return std::nullopt;
    return found->second;
}

std::string LiveSessionView::local_faction() const {
    return player_faction(player_);
}

} // namespace eawr::presentation::godot_backend
