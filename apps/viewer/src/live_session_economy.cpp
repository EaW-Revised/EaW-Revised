#include "eawr/battle/start.hpp"
#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "settings_store.hpp"
#include "eawr/presentation/ui/gc_battle_colours.hpp"
#include "eawr/presentation/ui/pads.hpp"
#include "eawr/presentation/ui/skirmish_setup.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

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

std::vector<ui::ObserverOwner> LiveSessionView::observer_owners() const {
    std::vector<ui::ObserverOwner> result;
    if (!start_) return result;
    const auto lobby = options_.lan ? options_.lan->lobby_state() : std::nullopt;
    std::vector<ui::ObserverOwner> seats;
    if (lobby) {
        for (const auto& seat : lobby->snapshot.seats)
            seats.push_back({seat.player ? seat.player : seat.selection.slot, seat.selection.name, {}, {}});
    }
    for (const auto& [player, name] : options_.observer_names) {
        const auto count = std::count_if(seats.begin(), seats.end(),
            [&](const auto& seat) { return seat.player == player; });
        if (count == 0) seats.push_back({player, name, {}, {}});
        else if (count == 1) {
            const auto seat = std::find_if(seats.begin(), seats.end(),
                [&](const auto& candidate) { return candidate.player == player; });
            if (seat->name.empty()) seat->name = name;
        }
    }
    const auto* controller_policy = battle_frame_.latest && battle_frame_.latest->controller_policy()
        ? &*battle_frame_.latest->controller_policy() : setup_ && setup_->controllers ? &*setup_->controllers : nullptr;
    for (const auto& entry : start_->players) {
        std::string name(ui::resolve_owner_name(entry.player.player_id, seats, entry.faction,
            entry.lobby ? std::string_view{} : std::string_view(entry.faction), controller_policy));
        std::string text_key;
        if (name.empty() && !entry.human && entry.lobby)
            text_key = ui::skirmish_setup_text_key(entry.difficulty == tactical::AiLevel::easy
                ? ui::SetupText::ai_easy_player : entry.difficulty == tactical::AiLevel::hard
                ? ui::SetupText::ai_hard_player : ui::SetupText::ai_player);
        result.push_back({entry.player.player_id, std::move(name),
            participant_colour({}, entry.player.player_id).value_or(ui::Rgb{255, 255, 255}), std::move(text_key)});
    }
    if (controller_policy) {
        for (const auto& binding : controller_policy->seats) {
            const auto identity = std::find_if(options_.controller_names.begin(), options_.controller_names.end(),
                [&](const auto& row) { return row.first == binding.seat; });
            const auto fallback = std::find_if(result.begin(), result.end(),
                [&](const auto& row) { return row.player == binding.player; });
            ui::ObserverOwner owner{binding.player, identity != options_.controller_names.end() ? identity->second
                : fallback != result.end() ? fallback->name : std::string{},
                participant_colour(binding.seat, binding.player).value_or(ui::Rgb{255, 255, 255}), {}, binding.seat};
            if (binding.kind == sim::SeatKind::ai && identity == options_.controller_names.end()) {
                owner.name.clear();
                owner.text_key = ui::skirmish_setup_text_key(ui::SetupText::ai_player);
            } else if (owner.name.empty() && fallback != result.end()) owner.text_key = fallback->text_key;
            result.push_back(std::move(owner));
        }
    } else if (options_.borrowed_battle) {
        // The borrowed feed may publish its controller policy after the UI is prepared.
        for (const auto& binding : options_.colour_seats) {
            if (!binding.active) continue;
            const auto identity = std::find_if(options_.controller_names.begin(), options_.controller_names.end(),
                [&](const auto& row) { return row.first == binding.seat_id; });
            const auto fallback = std::find_if(result.begin(), result.end(),
                [&](const auto& row) { return row.player == binding.player && row.seat.value == 0; });
            if (fallback == result.end()) continue;
            ui::ObserverOwner owner{binding.player, identity != options_.controller_names.end() ? identity->second : fallback->name,
                participant_colour(binding.seat_id, binding.player).value_or(ui::Rgb{255, 255, 255}),
                fallback->text_key, binding.seat_id};
            if (binding.kind == sim::SeatKind::ai && identity == options_.controller_names.end()) {
                owner.name.clear();
                owner.text_key = ui::skirmish_setup_text_key(ui::SetupText::ai_player);
            }
            result.push_back(std::move(owner));
        }
    }
    return result;
}

bool LiveSessionView::can_control(const tactical::TacticalInstance& instance) const noexcept {
    if (options_.view_only) return false;
    if (!controller_policy_enabled()) return true;
    // Existing authored community/pad rights survive the mobile-seat gate.
    bool shared = instance.control == tactical::UnitControl::shared_structure && is_ally_of_local(instance.owner);
    if (instance.control == tactical::UnitControl::none && is_ally_of_local(instance.owner)) {
        const auto type = presentation_types_.find(instance.type_id);
        shared = type != presentation_types_.end() && type->second->station_community_property;
        if (const auto* point = economy_.pads.point(instance.type_id)) shared = shared || point->community_property || point->build_pad;
    }
    return ui::can_control(options_.controller_seat, instance, battle_frame_.latest->controller_policy(), shared);
}

const std::vector<ui::ObserverOwner>& LiveSessionView::gift_recipients() const {
    if (!controller_policy_enabled()) {
        gift_policy_.reset(); gift_roster_.clear();
        return gift_roster_;
    }
    const auto& policy = battle_frame_.latest->controller_policy();
    if (gift_policy_ == policy) return gift_roster_;
    gift_policy_ = policy;
    gift_roster_.clear();
    const auto eligible = ui::gift_recipients(options_.controller_seat, policy);
    const auto owners = observer_owners();
    for (const auto seat : eligible) {
        const auto owner = std::find_if(owners.begin(), owners.end(), [seat](const auto& row) { return row.seat == seat; });
        if (owner != owners.end() && !owner->name.empty()) gift_roster_.push_back(*owner);
    }
    return gift_roster_;
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::player_colour(const tactical::PlayerId player) const {
    if (!start_) return std::nullopt;
    for (const battle::StartPlayer& entry : start_->players) {
        if (entry.player.player_id == player && entry.colour) return entry.colour->rgb;
    }
    return std::nullopt;
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::owner_ui_colour(const tactical::PlayerId player) const {
    // WSU-31: the player's colour in a multiplayer tactical game (a skirmish), else the faction's.
    if (colorization_enabled_) return player_colour(player);
    const auto faction = ui::faction_colour(faction_colours_, player_faction(player));
    if (!faction) return std::nullopt;
    return std::array<std::uint8_t, 3>{faction->r, faction->g, faction->b};
}

void LiveSessionView::ensure_gc_seat_colours() const {
    if (gc_seat_colours_ready_ || !options_.borrowed_battle || !start_ || team_of_player_.empty()) return;
    gc_seat_colours_ready_ = true;
    gc_seat_colours_.clear();
    // #8355: the local seat and every enemy keep their faction's own look (no entry); the local side's mates, human or
    // allied AI, get tinted variants of the local faction's colour. Developer workshop switch:
    // EAWR_GC_ALLY_OWN_COLOURS=1 gives each human mate a palette colour of its own instead of a shade.
    std::optional<std::array<std::uint8_t, 3>> own;
    const auto local = std::find_if(options_.colour_seats.begin(), options_.colour_seats.end(),
        [&](const auto& row) { return row.seat_id == options_.controller_seat; });
    if (local != options_.colour_seats.end()) {
        if (const auto faction = ui::faction_colour(faction_colours_, player_faction(local->player), true))
            own = std::array<std::uint8_t, 3>{faction->r, faction->g, faction->b};
    }
    const bool shades = godot::OS::get_singleton()->get_environment("EAWR_GC_ALLY_OWN_COLOURS") != godot::String("1");
    std::vector<sim::SeatId> ai_mates;
    for (const auto& seat : options_.colour_seats)
        if (seat.active && seat.kind == sim::SeatKind::ai && seat.player != player_ && is_ally_of_local(seat.player))
            ai_mates.push_back(seat.seat_id);
    for (const auto& seat : options_.colour_seats)
        if (const auto colour = ui::gc_participant_colour(seat.seat_id, options_.colour_seats, options_.colour_teams,
                gc_palette_, shades, options_.controller_seat, own, ai_mates)) gc_seat_colours_.emplace(seat.seat_id, *colour);
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::participant_colour(
        sim::SeatId seat, const tactical::PlayerId owner) const {
    if (options_.borrowed_battle) {
        ensure_gc_seat_colours();
        if (seat.value == 0) {
            const auto found = std::find_if(options_.colour_seats.begin(), options_.colour_seats.end(),
                [owner](const auto& row) { return row.player == owner && row.active && row.kind == sim::SeatKind::human; });
            if (found != options_.colour_seats.end()) seat = found->seat_id;
        }
        if (const auto colour = gc_seat_colours_.find(seat); colour != gc_seat_colours_.end()) return colour->second;
    }
    return owner_ui_colour(owner);
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::instance_model_colour(
        const tactical::TacticalInstance& instance, const std::string& object_id) const {
    const auto faction = model_colour(instance.owner, object_id);
    const bool participant_units = gc_participant_units() && !neutral_model_owners_.contains(instance.owner);
    if (participant_units) ensure_gc_seat_colours();
    return ui::gc_instance_model_colour(participant_units, instance, faction, options_.colour_seats, gc_seat_colours_);
}

void LiveSessionView::apply_gc_colour_toggle() {
    if (gc_follow_mate_ && battle_frame_.latest) {
        ensure_gc_seat_colours();
        for (const auto& instance : battle_frame_.latest->instances()) {
            if (!gc_seat_colours_.contains(instance.controller_seat) || !composed_ship(instance.entity_id)) continue;
            options_.follow = instance.entity_id;
            gc_follow_mate_ = false;
            break;
        }
    }
    if (!gc_colour_toggle_ || presented_tick_ < gc_colour_toggle_->first) return;
    const auto* saved = ActiveSettings::get();
    if (saved) {
        for (const auto& ship : placed_ships_) {
            if (!ship.live_entity || !ship.team_colour) continue;
            gc_colours_before_toggle_ += (gc_colours_before_toggle_.empty() ? "" : ", ") + std::string("{\"entity\": ")
                + std::to_string(ship.live_entity) + ", \"rgb\": [" + std::to_string((*ship.team_colour)[0]) + ", "
                + std::to_string((*ship.team_colour)[1]) + ", " + std::to_string((*ship.team_colour)[2]) + "]}";
        }
        auto changed = *saved;
        changed.gameplay.gc_battle_colours = gc_colour_toggle_->second;
        ActiveSettings::set(std::move(changed));
    }
    gc_colour_toggle_.reset();
}

bool LiveSessionView::gc_participant_units() const noexcept {
    const auto* saved = ActiveSettings::get();
    return options_.borrowed_battle && saved && saved->gameplay.gc_battle_colours == 1;
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::minimap_unit_colour(
        const tactical::TacticalInstance& instance, const std::string& object_id) const {
    if (!gc_participant_units()) return std::nullopt;
    return instance_model_colour(instance, object_id);
}

bool LiveSessionView::model_uses_player_colour(const tactical::PlayerId player) const noexcept {
    return colorization_enabled_ && !neutral_model_owners_.contains(player);
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::model_colour(const tactical::PlayerId player,
                                                                         const std::string& object_id) const {
    if (model_uses_player_colour(player)) return player_colour(player);
    if (const auto type = type_no_colorization_.find(object_id); type != type_no_colorization_.end()) return type->second;
    const auto faction = ui::faction_colour(faction_colours_, player_faction(player), true);
    if (!faction) return std::nullopt;
    return std::array<std::uint8_t, 3>{faction->r, faction->g, faction->b};
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::model_colour_of_type(const tactical::PlayerId player,
                                                                                 const sim::tactical::TypeId type_id) const {
    // WBP-52: a pad's capture progress feeds the model's colourisation constant, so it takes the model's colour.
    if (tables_) {
        // presentation_types_ holds every unit of the table once its frame indexing has run (first match wins, like the
        // scan below), so a lookup replaces one string CRC per unit; the scan covers the frames before that.
        if (!presentation_types_.empty()) {
            const auto found = presentation_types_.find(type_id);
            return found != presentation_types_.end() ? model_colour(player, found->second->id) : model_colour(player, std::string{});
        }
        for (const auto& unit : tables_->units) {
            if (battle::type_id(unit.id) == type_id) return model_colour(player, unit.id);
        }
    }
    return model_colour(player, std::string{});
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
    ui::TacticalIntent pending; pending.verb = ui::TacticalVerb::pad_build; pending.units = {entity};
    if (command_pending(pending)) return false;
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
    // An unsupported ability: a kind the unit's type authors keeps its button, never usable.
    if (kind == tactical::AbilityKind::none) return ui::UnitAbilityState{ui::AbilityStatus::disabled, 1.0, false};
    ui::TacticalIntent pending;
    pending.verb = ui::TacticalVerb::ability; pending.units = {unit}; pending.unit_ability = kind;
    if (view_.command_pending(pending)) return ui::UnitAbilityState{ui::AbilityStatus::disabled, 0.0, false};
    const auto& snapshot = view_.ability_snapshot_;
    if (!snapshot) return std::nullopt;
    const std::array<sim::EntityId, 1> single{unit};
    std::span<const sim::EntityId> holders = single;
    // A squadron's container stands for its craft (AB-15), except for a team ability, which the
    // container holds itself (#561, AB-60; WAB-70 for BUZZ_DROIDS).
    const auto squadron = view_.squadron_members_.find(unit);
    if (squadron != view_.squadron_members_.end() && kind != tactical::AbilityKind::ion_cannon_shot
        && kind != tactical::AbilityKind::buzz_droids && kind != tactical::AbilityKind::lucky_shot) {
        holders = squadron->second;
    }
    bool has = false;
    bool all_active = true;
    bool all_autofire = true;
    bool blocked = false;
    std::optional<double> recharge;
    bool running_countdown = false; // WLD-10
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
            if (status.active && status.countdown_total_frames > 0) {
                // WLD-10: a countdown running during the run is shown as the recharge fill, not the run.
                const double done = 1.0 - static_cast<double>(status.countdown_remaining_frames) / status.countdown_total_frames;
                recharge = recharge ? std::min(*recharge, done) : done;
                running_countdown = true;
            } else if (status.active && status.total_frames > 0) {
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
    if (all_active && running_countdown && recharge) {
        result.status = ui::AbilityStatus::recharging;
        result.recharge = std::clamp(*recharge, 0.0, 1.0);
    } else if (all_active) {
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
    if (kind == tactical::AbilityKind::none || (request.targeted && !aimed) || !view_.scheduler_ || view_.view_only()) {
        ++refused_;
        return;
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::ability;
    intent.units = request.units;
    if (view_.controller_policy_enabled()) std::erase_if(intent.units, [&](const auto id) { return !view_.can_control(id); });
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
    if ((view_.input_sink_ ? *view_.input_sink_ : *view_.scheduler_).issue(intent)) {
        ++issued_;
        if (intent.ability_action == tactical::AbilityAction::activate
            || intent.ability_action == tactical::AbilityAction::deactivate) {
            view_.ability_clicks_.push_back({kind, intent.ability_action == tactical::AbilityAction::activate, request.units, aimed});
        }
    } else {
        ++refused_;
    }
}

const tactical::EconomyView* LiveSessionView::local_economy() const {
    if (!battle_frame_.latest) return nullptr;
    for (const tactical::EconomyView& view : battle_frame_.latest->economy()) {
        if (view.player != player_) continue;
        const auto revision = pending_revision();
        if (pending_economy_state_.tick != battle_frame_.latest->completed_tick()
            || pending_economy_state_.revision != revision) {
            const auto rows = scheduler_ ? scheduler_->reservations() : std::vector<ui::PendingCommand>{};
            if (rows.empty()) pending_economy_state_.view.reset();
            else pending_economy_state_.view = ui::pending_economy(view, rows, [this](const sim::EntityId station, const tactical::TypeId type) -> const tactical::BuildOption* {
                if (station) {
                    const auto* instance = space::find_instance(*battle_frame_.latest, station);
                    const auto* menu = instance ? producer_menu(*instance) : nullptr;
                    return menu ? menu->find(type) : nullptr;
                }
                for (const auto& menu : economy_.menus) if (const auto* option = menu.find(type)) return option;
                return nullptr;
            }, battle_frame_.latest->completed_tick());
            pending_economy_state_.tick = battle_frame_.latest->completed_tick();
            pending_economy_state_.revision = revision;
        }
        return pending_economy_state_.view ? &*pending_economy_state_.view : &view;
    }
    return nullptr;
}

const tactical::GarrisonTimerView* LiveSessionView::local_garrison_timer() const noexcept {
    if (!battle_frame_.latest) return nullptr;
    for (const auto& timer : battle_frame_.latest->garrison_timers())
        if (timer.player == player_) return &timer;
    return nullptr;
}

bool LiveSessionView::build_allowed(const tactical::BuildOption& option) const {
    return build_menu_state(option).enabled;
}

ui::BuildOptionState LiveSessionView::build_menu_state(const tactical::BuildOption& option) const {
    if (session_) {
        return ui::build_option_state(option, [&](const tactical::TypeId type) {
            const auto view = session_->production_view(player_, type);
            // Read execution assignments after the live snapshot: the driver may
            // hand off a command while production_view waits for the session lock.
            const auto rows = scheduler_ ? scheduler_->reservations() : std::vector<ui::PendingCommand>{};
            if (!view.snapshot || rows.empty()) return view.counts;
            for (const auto& ledger : view.snapshot->economy()) {
                if (ledger.player != player_) continue;
                return ui::pending_production_counts(view.counts, ledger, view.snapshot->completed_tick(), rows,
                    [&](const sim::EntityId station, const tactical::TypeId queued_type) -> const tactical::BuildOption* {
                        const auto* instance = space::find_instance(*view.snapshot, station);
                        const auto* menu = instance ? producer_menu(*instance) : nullptr;
                        return menu ? menu->find(queued_type) : nullptr;
                    }, type);
            }
            return view.counts;
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

const tactical::StationMenu* LiveSessionView::producer_menu(const tactical::TacticalInstance& instance) const noexcept {
    const auto local = local_faction_id();
    if (!local || !setup_) return nullptr;
    auto owner = *local;
    for (const tactical::Player& player : setup_->players) {
        if (player.player_id == instance.owner) { owner = player.faction_id; break; }
    }
    return economy_.producer_menu(instance.type_id, owner, *local);
}

namespace {

// #530: an economy intent through the order scheduler; false when the scheduler refused it.
bool issue_economy(ui::CommandSink* scheduler, const ui::TacticalIntent& intent) {
    return scheduler != nullptr && static_cast<bool>(scheduler->issue(intent));
}

} // namespace

bool LiveSessionView::pad_sale_allowed(const sim::EntityId child, const bool single_step) const {
    ui::TacticalIntent pending; pending.verb = ui::TacticalVerb::pad_sell; pending.units = {child};
    if (command_pending(pending)) return false;
    const auto* instance = snapshot_index_.instance(child);
    if (instance == nullptr || !battle_frame_.latest || local_economy() == nullptr) return false;
    const auto parents = battle_frame_.latest->pads();
    const auto parent = std::find_if(parents.begin(), parents.end(),
        [child](const auto& pad) { return pad.state.constructed == child; });
    return tactical::pad_sale_permission(player_, instance->owner, economy_.pad_sale(instance->type_id) != nullptr,
        parent != parents.end() ? &parent->state : nullptr, single_step);
}

bool LiveSessionView::sell_pad_structure(const sim::EntityId child, const bool single_step) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    if (!pad_sale_allowed(child, single_step)) return false;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::pad_sell;
    intent.units = {child};
    intent.origin = ui::CommandOrigin::world_click;
    return issue_economy(command_sink(), intent);
}

bool LiveSessionView::special_fire(const sim::EntityId source, const sim::EntityId target) {
    if (phase_ != Phase::running || options_.view_only || (controller_policy_enabled() && !can_control(source))) return false;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::special_fire;
    intent.units = {source};
    intent.target = target;
    return issue_economy(command_sink(), intent);
}

bool LiveSessionView::superweapon_request(const sim::EntityId unit, const sim::tactical::TswStage stage) {
    if (phase_ != Phase::running || options_.view_only || (controller_policy_enabled() && !can_control(unit))) return false;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::tactical_superweapon;
    intent.units = {unit};
    intent.tsw_stage = stage;
    intent.origin = ui::CommandOrigin::hud_button;
    if (command_pending(intent)) return false; // the same click is already waiting for its tick
    return issue_economy(command_sink(), intent);
}

bool LiveSessionView::hardpoint_repair_allowed(const sim::EntityId station, const std::uint32_t hardpoint) const {
    ui::TacticalIntent pending; pending.verb = ui::TacticalVerb::repair_hardpoint; pending.units = {station}; pending.hardpoint = hardpoint;
    if (command_pending(pending)) return false;
    const auto* instance = snapshot_index_.instance(station);
    if (!instance || !instance->durability || !local_economy()) return false;
    // WSL-40/USL-03: ordinary own selection or allied community-station selection.
    // Repair adds no second owner gate after that admission (debug build EUS-01).
    if (instance->owner != player_) {
        const auto type = presentation_types_.find(instance->type_id);
        if (type == presentation_types_.end() || !type->second->station_community_property
            || !is_ally_of_local(instance->owner)) return false;
    }
    if (std::none_of(economy_.menus.begin(), economy_.menus.end(),
        [&](const tactical::StationMenu& menu) { return menu.station == instance->type_id; })) return false;
    if (hardpoint >= instance->durability->hardpoints.size()) return false;
    const auto& slot = instance->durability->hardpoints[hardpoint];
    // HR-05 recovery: automatic healing can fill a disabled slot before paid repair completes it.
    return slot.max_health.raw() > 0 && slot.health.raw() > 0 && (slot.health < slot.max_health || !slot.enabled)
        && std::find(slot.repairing_players.begin(), slot.repairing_players.end(), player_) == slot.repairing_players.end();
}

bool LiveSessionView::repair_hardpoint(const sim::EntityId station, const std::uint32_t hardpoint) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    if (!hardpoint_repair_allowed(station, hardpoint)) return false;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::repair_hardpoint;
    intent.units = {station};
    intent.hardpoint = hardpoint;
    intent.origin = ui::CommandOrigin::world_click;
    return issue_economy(command_sink(), intent);
}

bool LiveSessionView::buy(const sim::EntityId station, const tactical::TypeId type) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
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
    const auto* instance = snapshot_index_.instance(station);
    const auto* menu = instance ? producer_menu(*instance) : nullptr;
    const auto* option = menu ? menu->find(type) : nullptr;
    const auto* ledger = local_economy();
    if (!option || !ledger || ledger->credits < option->price || !build_allowed(*option)
        || (intent.verb == ui::TacticalVerb::buy && ledger->queues[static_cast<std::size_t>(option->queue)].size() >= economy_.max_queue)) {
        ++economy_requests_.refused; return false;
    }
    intent.units = {station};
    intent.type = type;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(command_sink(), intent);
    ++(issued ? economy_requests_.buys : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::cancel_build(const tactical::BuildQueue queue, const std::uint64_t entry_id) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    if (entry_id == 0) { ++economy_requests_.refused; return false; }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::cancel;
    intent.queue = queue;
    intent.queue_entry_id = entry_id;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(command_sink(), intent);
    ++(issued ? economy_requests_.cancels : economy_requests_.refused);
    return issued;
}

sim::math::Fixed LiveSessionView::reinforcement_facing() const noexcept {
    const auto* player = economy_.player(player_);
    return deployment_facing_.value_or(player ? player->reinforcement_yaw : sim::math::Fixed{});
}

bool LiveSessionView::rotate_reinforcement(const double detents) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    if (!reinforcement_allowed() || !std::isfinite(detents) || detents == 0.0) return false;
    constexpr double degrees_per_detent = 15.0; // WR-X01: owner-requested project constant
    const auto step = scene::fixed_from_binary32(static_cast<float>(std::fmod(detents * degrees_per_detent, 360.0)));
    if (!step) return false;
    constexpr auto turn = 360 * sim::math::Fixed::scale;
    auto yaw = (reinforcement_facing().raw() % turn + step.value().raw()) % turn;
    if (yaw < 0) yaw += turn;
    deployment_facing_ = sim::math::Fixed::from_raw(yaw);
    preview_valid_ = false;
    preview_checked_tick_.reset();
    return true;
}

bool LiveSessionView::reinforce(const tactical::TypeId type, const sim::math::Vec3& point) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    // #7190: on land the simulation alone admits a company (cap, then the whole company at the point); a drop it refuses comes
    // back as an order-rejected event with its own reason, which the reinforcement pane shows. So only the pool entry is
    // checked here, and the command carries the click whatever the preview said.
    if (land_terrain() != nullptr) {
        const auto* pooled = local_economy();
        if (!reinforcement_allowed() || pooled == nullptr
            || std::find(pooled->pool.begin(), pooled->pool.end(), type) == pooled->pool.end()) {
            ++economy_requests_.refused;
            return false;
        }
    } else {
        // WR-15: drop checks placement before room. A busy preview query has no authority;
        // the cached same-point verdict may be used, and the command independently rechecks state.
        const auto valid = session_ ? session_->reinforcement_point(player_, type, point, deployment_facing_) : std::optional<bool>{false};
        const bool cached = preview_type_ == type && preview_point_ == point && preview_valid_;
        if (!reinforcement_allowed() || !valid.value_or(cached) || !reinforcement_room(type)) {
            ++economy_requests_.refused;
            return false;
        }
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::reinforce;
    intent.type = type;
    intent.destination = point;
    intent.reinforcement_facing = deployment_facing_;
    // WGM-48: grouped type slots still deploy an existing reserve, never a new
    // type-only purchase. Pending commands claim distinct published identities.
    const auto* ledger = local_economy();
    const auto* reserve = economy_.campaign ? campaign_reserve(type) : nullptr;
    if (reserve) intent.reinforcement_token = reserve->token;
    // Ordinary network skirmish admits the first pooled unit of a type; persistent
    // reserve identities require the campaign route's separate authorization.
    if (!economy_.campaign && !options_.lan && ledger) for (std::size_t index = 0; index < ledger->pool.size() && index < ledger->pool_tokens.size(); ++index) {
        if (ledger->pool[index] != type || ledger->pool_tokens[index] == 0) continue;
        const auto token = ledger->pool_tokens[index];
        intent.reinforcement_token = token;
        break;
    }
    if (economy_.campaign && intent.reinforcement_token == 0) {
        ++economy_requests_.refused;
        return false;
    }
    intent.origin = ui::CommandOrigin::world_click;
    const bool issued = issue_economy(command_sink(), intent);
    ++(issued ? economy_requests_.reinforcements : economy_requests_.refused);
    if (issued) {
        reinforcement_feedback(ReinforcementFeedback::Kind::enroute, type);
    }
    return issued;
}

void LiveSessionView::reinforcement_feedback(const ReinforcementFeedback::Kind kind, const tactical::TypeId type) {
    // WR-07/11/16: local presentation gestures/submissions, separate from authoritative arrival.
    if (reinforcement_allowed()) reinforcement_feedback_.push_back({kind, type});
}

std::vector<tactical::ReserveView> LiveSessionView::campaign_reinforcement_pool() const {
    const auto* ledger = local_economy();
    return ledger ? ledger->reserves : std::vector<tactical::ReserveView>{};
}

const tactical::ReserveView* LiveSessionView::campaign_reserve(const tactical::TypeId type) const noexcept {
    const auto* ledger = local_economy();
    if (!ledger) return nullptr;
    for (const auto& row : ledger->reserves) {
        if (row.type != type || !row.token || row.restricted || (deployment_token_ && row.token != deployment_token_)
            || ledger->population > ledger->population_cap
            || row.population > ledger->population_cap - ledger->population) continue;
        return &row;
    }
    return nullptr;
}

bool LiveSessionView::select_campaign_reinforcement(const std::uint64_t token) {
    if (options_.view_only) return false; // #8182: a watched battle issues no order
    deployment_token_ = token;
    const auto* ledger = local_economy();
    if (!economy_.campaign || !ledger || !token) return false;
    for (const auto& row : ledger->reserves) if (row.token == token) return campaign_reserve(row.type) != nullptr;
    return false;
}

std::uint32_t LiveSessionView::population_of(const tactical::TypeId type) const noexcept {
    if (economy_.campaign) for (const auto& [id, population] : economy_.campaign_population)
        if (id == type) return population;
    for (const auto& menu : economy_.menus) {
        if (const auto* option = menu.find(type)) return option->population;
    }
    return 0U;
}

std::vector<tactical::TypeId> LiveSessionView::reinforcement_pool() const {
    const auto* ledger = local_economy();
    return ledger ? ledger->pool : std::vector<tactical::TypeId>{};
}

std::vector<LiveSessionView::LandDeployRing> LiveSessionView::land_deploy_rings() const {
    std::vector<LandDeployRing> rings;
    const auto& latest = battle_frame_.latest;
    if (!land_content_ || !latest) return rings;
    const auto& rules = land_content_->land.reinforcement;
    if (!rules.enabled || !rules.use_points) return rings;
    const auto scale = static_cast<double>(sim::math::Fixed::scale);
    for (const auto& instance : latest->instances()) {
        const auto* point = rules.point(instance.type_id);
        if (point == nullptr || point->enabling_radius.raw() <= 0 || !is_ally_of_local(instance.owner)) continue;
        rings.push_back({static_cast<double>(instance.fixed_transform.rows[0][3].raw()) / scale,
            static_cast<double>(instance.fixed_transform.rows[1][3].raw()) / scale,
            static_cast<double>(point->enabling_radius.raw()) / scale});
    }
    return rings;
}

std::size_t LiveSessionView::pending_reinforcements() const {
    const auto rows = scheduler_ ? scheduler_->reservations() : std::vector<ui::PendingCommand>{};
    return static_cast<std::size_t>(std::count_if(rows.begin(), rows.end(),
        [](const auto& row) { return row.intent.verb == ui::TacticalVerb::reinforce; }));
}

bool LiveSessionView::reinforcement_allowed() const {
    if (!setup_ || outcome_ || local_economy() == nullptr) return false;
    const auto found = std::find_if(setup_->players.begin(), setup_->players.end(), [this](const tactical::Player& player) {
        return player.player_id == player_;
    });
    return found != setup_->players.end() && (found->flags & tactical::player_flag_commandable) != 0;
}

bool LiveSessionView::reinforcement_room(const tactical::TypeId type) const {
    const auto* ledger = local_economy();
    if (ledger == nullptr) return false;
    if (economy_.campaign) return campaign_reserve(type) != nullptr;
    const std::uint64_t population = ledger->population;
    if (std::find(ledger->pool.begin(), ledger->pool.end(), type) == ledger->pool.end()) return false;
    const std::uint32_t added = population_of(type);
    return population <= ledger->population_cap && added <= ledger->population_cap - population
        && (economy_.campaign || std::any_of(economy_.menus.begin(), economy_.menus.end(),
            [type](const auto& menu) { return menu.find(type) != nullptr; }));
}

void LiveSessionView::placement_preview(const std::optional<tactical::TypeId> type,
    const std::optional<sim::math::Vec3> point) {
    if (!type) deployment_token_ = 0;
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
    if (const auto valid = session_->reinforcement_point(player_, *type, *point, deployment_facing_)) {
        preview_valid_ = *valid;
        preview_checked_tick_ = tick;
    }
}

std::string LiveSessionView::player_faction(const tactical::PlayerId player) const {
    if (start_) {
        for (const battle::StartPlayer& candidate : start_->players) {
            if (candidate.player.player_id == player) return candidate.faction;
        }
        return {};
    }
    // Replays retain faction IDs rather than lobby names; all owners need arrival audio.
    if (!setup_) return {};
    for (const auto& candidate : setup_->players) {
        if (candidate.player_id != player) continue;
        for (const std::string_view name : {"Empire", "Rebel", "Underworld"}) {
            if (battle::faction_id(name) == candidate.faction_id) return std::string(name);
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
