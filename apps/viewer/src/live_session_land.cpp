#include "live_session_view.hpp"
#include "live_session_internal.hpp"
#include "eawr/platform/live_ai.hpp"
#include "eawr/skirmish/land_content.hpp"
#include "eawr/skirmish/land_start.hpp"
#include "eawr/scene/scene.hpp"
#include <godot_cpp/classes/json.hpp>
#include <system_error>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/array.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <set>
#include <stdexcept>

namespace eawr::presentation::godot_backend {
namespace {
using namespace godot;
using sim::math::Fixed;
namespace tactical = sim::tactical;

double number(const Variant& value) {
    if (value.get_type() != Variant::INT && value.get_type() != Variant::FLOAT)
        throw std::runtime_error("land bindings: expected a number");
    const double result = value;
    if (!std::isfinite(result)) throw std::runtime_error("land bindings: non-finite number");
    return result;
}
std::uint32_t integer(const Variant& value) {
    const double result = number(value);
    if (result < 0 || result > std::numeric_limits<std::uint32_t>::max() || std::floor(result) != result)
        throw std::runtime_error("land bindings: expected an unsigned integer");
    return static_cast<std::uint32_t>(result);
}
Fixed fixed(const Variant& value) {
    const double result = number(value) * Fixed::scale;
    if (result < 0 || result > static_cast<double>(std::numeric_limits<std::int64_t>::max() / 2))
        throw std::runtime_error("land bindings: physical value outside range");
    return Fixed::from_raw(static_cast<std::int64_t>(std::llround(result)));
}
Dictionary dictionary(const Variant& value) {
    if (value.get_type() != Variant::DICTIONARY) throw std::runtime_error("land bindings: expected an object");
    return value;
}
Array array(const Variant& value, int size = -1) {
    if (value.get_type() != Variant::ARRAY) throw std::runtime_error("land bindings: expected an array");
    const Array result = value;
    if (size >= 0 && result.size() != size) throw std::runtime_error("land bindings: incorrect array size");
    return result;
}
std::string string(const Variant& value) {
    if (value.get_type() != Variant::STRING) throw std::runtime_error("land bindings: expected a string");
    return String(value).utf8().get_data();
}

// R2/R3 and LR3-10 are explicit unverified gates. Physical locomotion,
// footprints and formation spacing always come from the authored L2b adapter.
skirmish::LandContentInputs read_bindings(const std::filesystem::path& path) {
    std::error_code code;
    const auto bytes = std::filesystem::file_size(path, code);
    if (code || bytes > 8 * 1024 * 1024) throw std::runtime_error("cannot read land bindings within byte limit");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read land bindings");
    std::string text((std::istreambuf_iterator<char>(input)), {});
    Ref<JSON> json; json.instantiate();
    if (json->parse(String::utf8(text.c_str())) != OK) throw std::runtime_error("invalid land bindings JSON");
    const auto root = dictionary(json->get_data());
    if (string(root["format"]) != "eawr-land-inputs-v1") throw std::runtime_error("unknown land bindings format");
    skirmish::LandContentInputs result;
    const auto masks = dictionary(root["unverified_passability_bits"]);
    const auto mask_keys = masks.keys();
    for (int64_t i = 0; i < mask_keys.size(); ++i) {
        const auto key = mask_keys[i];
        result.unverified_passability_bits.push_back({string(key), integer(masks[key])});
    }
    const auto collisions = dictionary(root["collision_class_order"]);
    const auto collision_keys = collisions.keys();
    for (int64_t i = 0; i < collision_keys.size(); ++i) {
        const auto key = collision_keys[i];
        result.collision_class_order.push_back({string(key), integer(collisions[key])});
    }
    const auto contacts = array(root["contacts"], 4);
    result.contacts = {integer(contacts[0]), integer(contacts[1]), fixed(contacts[2]), fixed(contacts[3])};
    const auto capacities = array(root["capacities"]);
    for (int64_t i = 0; i < capacities.size(); ++i) {
        const auto row = array(capacities[i], 2);
        if (!result.last_known_capacity.emplace(integer(row[0]), integer(row[1])).second)
            throw std::runtime_error("duplicate land capacity player");
    }
    const auto occupations = array(root["occupations"]);
    for (int64_t i = 0; i < occupations.size(); ++i) {
        const auto row = dictionary(occupations[i]);
        tactical::LandOccupation occupation;
        occupation.object = integer(row["object"]);
        occupation.primary_passability = integer(row["primary_passability"]);
        occupation.secondary_passability = integer(row["secondary_passability"]);
        const auto primary = array(row["primary_cells"]), secondary = array(row["secondary_cells"]);
        for (int64_t cell = 0; cell < primary.size(); ++cell) occupation.primary_cells.push_back(integer(primary[cell]));
        for (int64_t cell = 0; cell < secondary.size(); ++cell) occupation.secondary_cells.push_back(integer(secondary[cell]));
        result.occupations.push_back(std::move(occupation));
    }
    return result;
}

// WLAN-10..12: the authored locomotor class of the speeder bike (a bike lean, fixed move rate and roll limit); the member's
// locomotor class name is the data's, compared by type id.
constexpr std::string_view bike_locomotor = "BIKE_LOCOMOTOR";

} // namespace

bool LiveSessionView::prepare_land(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    std::string_view map_path, std::string& failure) {
    try {
        if (options_.fixture != "skirmish" || options_.lan || options_.borrowed_battle)
            throw std::runtime_error("land live view requires a standalone skirmish start");
        if (options_.skirmish.map && live_session_detail::lower_path(*options_.skirmish.map) != live_session_detail::lower_path(map_path))
            throw std::runtime_error("land skirmish map and viewed map differ");
        if (!options_.skirmish.slots) throw std::runtime_error("land live view requires explicit human --eawr-skirmish-slots");
        skirmish::Fixture fixture;
        fixture.map = map_path; fixture.slots = *options_.skirmish.slots;
        fixture.seed = options_.skirmish.seed.value_or(0); fixture.match = options_.skirmish.match;
        // #3207 G1-b: a non-human lobby slot runs the land AI (the land mode of the FoC host) unless the AI is off.
        const bool ai_enabled = options_.ai.value_or(true);
        if (!ai_enabled && std::any_of(fixture.slots.begin(), fixture.slots.end(), [](const auto& slot) { return !slot.human; }))
            throw std::runtime_error("land live view P1 requires human lobby slots");
        if (std::none_of(fixture.slots.begin(), fixture.slots.end(), [](const auto& slot) { return slot.human; }))
            throw std::runtime_error("land live view needs at least one human lobby slot");
        units::UnitTables empty;
        auto discovered = skirmish::read_land_start_inputs(fixture, filesystem, catalog, empty);
        if (!discovered) throw std::runtime_error(core::format_diagnostic(discovered.error()));
        skirmish::set_land_vehicles_only(discovered.value(), options_.land_vehicles_only);
        fixture.map_sha256 = discovered.value().common.map_sha256;
        scene::VfsAssetCache cache(filesystem);
        units::LoadInput load;
        load.domain = tactical::BattleDomain::land; load.catalog = &catalog; load.filesystem = &filesystem;
        load.model = cache.access().model;
        std::set<std::string> requested;
        for (const auto& slot : fixture.slots) {
            const auto faction = std::find_if(discovered.value().factions.begin(), discovered.value().factions.end(),
                [&](const auto& row) { return row.faction == slot.faction; });
            if (faction == discovered.value().factions.end()) continue;
            requested.insert(faction->forces.begin(), faction->forces.end());
            requested.insert(faction->vehicle_forces.begin(), faction->vehicle_forces.end());
        }
        if (requested.empty()) throw std::runtime_error("selected land factions have no authored starting companies");
        load.types.assign(requested.begin(), requested.end());
        auto tables = units::load_unit_tables(load);
        if (!tables) throw std::runtime_error(core::format_diagnostic(tables.error()));
        for (const auto& company : tables.value().land_companies)
            for (const auto& member : company.members) requested.insert(member.type);
        load.types.assign(requested.begin(), requested.end());
        tables = units::load_unit_tables(load);
        if (!tables) throw std::runtime_error(core::format_diagnostic(tables.error()));
        auto inputs = skirmish::read_land_start_inputs(fixture, filesystem, catalog, tables.value());
        if (!inputs) throw std::runtime_error(core::format_diagnostic(inputs.error()));
        skirmish::set_land_vehicles_only(inputs.value(), options_.land_vehicles_only);
        // No bindings file (menu start): the authored adapter supplies the gates; the file is an override.
        skirmish::LandContentInputs bindings;
        if (options_.land_bindings.empty()) {
            auto authored = skirmish::authored_land_content_inputs(tables.value());
            if (!authored) throw std::runtime_error(core::format_diagnostic(authored.error()));
            bindings = std::move(authored.value());
        } else bindings = read_bindings(options_.land_bindings);
        auto start = skirmish::build_land_start(fixture, inputs.value(), bindings);
        if (!start) throw std::runtime_error(core::format_diagnostic(start.error()));
        // #3207 G1-b: the AI players of the land start. The scripted session wraps the land TacticalSession in the
        // session start (LiveSession::Options::scripts); the closure, template and difficulty come from the mounted data.
        const auto& land_start = start.value();
        if (std::any_of(land_start.start.players.begin(), land_start.start.players.end(),
                [](const auto& player) { return player.lobby && !player.human; })) {
            auto ai = platform::live_land_ai(land_start, inputs.value(), tables.value(), filesystem);
            if (!ai) throw std::runtime_error("land AI scripts: " + core::format_diagnostic(ai.error()));
            for (const auto& note : ai.value().load_notes) std::cerr << core::format_diagnostic(note) << std::endl;
            ai_scripts_ = std::move(ai.value().scripts);
            ai_players_ = std::move(ai.value().players);
        }
        start_ = std::move(start.value().start);
        setup_ = start_->setup;
        land_content_ = std::move(start.value().content);
        content_.emplace(); // Land P1 has no combat, fog or economy UI; the AI (G1-b) is mounted above.
        economy_ = land_content_->economy; victory_ = land_content_->victory;
        tables_ = std::move(tables).value();
        // #6992: the viewer's tables hold the company types only; the owned pads, their menu structures and the pre-built
        // children are decoded in the start inputs. Merge them (the start build does the same for the bindings) so the
        // presentation, the pick shapes and the slot growth below know their models and selection flags.
        skirmish::merge_land_pad_types(*tables_, inputs.value().pad_tables);
        // #6992: the structure types a pad can complete into (the under-construction type and the finished one) get a draw
        // slot when they first appear; the slot growth for a land session reads this set (grow_launch_slot).
        land_structure_types_.clear();
        for (const auto& menu : economy_.menus) {
            if (economy_.pads.point(menu.station) == nullptr) continue;
            for (const auto& option : menu.options) {
                const auto* child = economy_.pads.child(option.type);
                if (child == nullptr) continue;
                land_structure_types_.insert(child->type);
                land_structure_types_.insert(child->constructed);
            }
        }
        arrival_slots_.filesystem = &filesystem;
        arrival_slots_.catalog = &catalog;
        player_ = options_.player.value_or(std::find_if(fixture.slots.begin(), fixture.slots.end(),
            [](const auto& slot) { return slot.human; })->slot); // the first human (one exists, checked above)
        if (std::none_of(setup_->players.begin(), setup_->players.end(), [&](const auto& p) { return p.player_id == player_; }))
            throw std::runtime_error("land local player is not in the start");
        for (const auto& player : setup_->players) team_of_player_[player.player_id] = player.team_id;
        options_.reveal = true; // Land fog presentation belongs to L6 P2.
        shield_flash_constants_ = presentation_constants::load_flash(filesystem);
        order_flash_constants_ = presentation_constants::load_order_flash(filesystem);
        return true;
    } catch (const std::exception& error) { failure = error.what(); return false; }
}

void LiveSessionView::bind_land_roster(const platform::LiveSession& session) {
    land_playback_.clear();
    land_last_shot_.clear();
    land_playback_tick_ = 0;
    land_playback_started_ = false;
    const auto zero = session.frame().latest;
    for (const auto& instance : zero->instances()) owner_of_entity_[instance.entity_id] = instance.owner;
    for (const auto& [member, root] : session.land_roster()) {
        squadron_of_[member] = root;
        squadron_members_[root].push_back(member);
        const auto instances = zero->instances();
        const auto found = std::lower_bound(instances.begin(), instances.end(), member,
            [](const auto& instance, auto id) { return instance.entity_id < id; });
        if (found == instances.end() || found->entity_id != member) continue;
        const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
            [&](const auto& row) { return skirmish::type_id(row.id) == found->type_id; });
        if (type == tables_->units.end()) continue;
        BattleScene::PlacedUnit placed;
        placed.land_motion = true;
        auto& playback = land_playback_[member];
        for (const auto& company : tables_->land_companies) {
            bool found_member = false;
            for (std::size_t index = 0; index < company.members.size() && index < company.locomotion.size(); ++index) {
                if (skirmish::type_id(company.members[index].type) != found->type_id) continue;
                playback.data = company.locomotion[index];
                playback.bike = skirmish::type_id(company.members[index].locomotor) == skirmish::type_id(bike_locomotor);
                playback.team = company.members.size() > 1 || company.members.front().create_team;
                found_member = true;
                break;
            }
            if (found_member) break;
        }
        placed.object_id = type->id; placed.live_entity = member;
        placed.position = {live_session_detail::to_float(found->fixed_transform.rows[0][3]), live_session_detail::to_float(found->fixed_transform.rows[1][3]),
            live_session_detail::to_float(found->fixed_transform.rows[2][3])};
        placed.yaw_degrees = static_cast<float>(live::instance_yaw_degrees(found->fixed_transform));
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& p) { return p.player.player_id == found->owner; });
        if (owner != start_->players.end() && owner->colour) placed.team_colour = owner->colour->rgb;
        ship_of_entity_[member] = placed_ships_.size();
        placed_ships_.push_back(std::move(placed));
    }
    // #6992: the owned pads and the pre-built structures stand in the start as station units. They are drawn from the
    // session like the space path's start units (a ship bound to the entity), so each has a pick volume and a click can
    // select it or open a pad's palette. The map's own marker records are dropped from the static composition
    // (strip_session_owned_placements) so a structure is not drawn twice.
    // The binding is the space path's (battle_source_local, every start unit): the same containers, one ship per entity. It is
    // built here instead of shared because the pose comes from the tick-zero snapshot instance (as for the roster members above,
    // so the drawn pose is the simulated one), not from the start unit; the colour is the owner's, and there is no map-record
    // colour fallback. The record list (land_session_records_) names only what was bound.
    std::map<sim::EntityId, std::uint32_t> structures; // entity -> the map record it started on
    for (const auto& unit : start_->units) if (unit.role == battle::UnitRole::station) structures.emplace(unit.state.entity_id, unit.record);
    land_session_records_.clear();
    for (const auto& instance : zero->instances()) {
        if (!structures.contains(instance.entity_id) || ship_of_entity_.contains(instance.entity_id)) continue;
        const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
            [&](const auto& row) { return skirmish::type_id(row.id) == instance.type_id; });
        if (type == tables_->units.end()) continue; // no table row: it keeps its map record's own drawing
        BattleScene::PlacedUnit placed;
        placed.object_id = type->id; placed.live_entity = instance.entity_id;
        placed.position = {live_session_detail::to_float(instance.fixed_transform.rows[0][3]), live_session_detail::to_float(instance.fixed_transform.rows[1][3]),
            live_session_detail::to_float(instance.fixed_transform.rows[2][3])};
        placed.yaw_degrees = static_cast<float>(live::instance_yaw_degrees(instance.fixed_transform));
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const auto& p) { return p.player.player_id == instance.owner; });
        if (owner != start_->players.end() && owner->colour) placed.team_colour = owner->colour->rgb;
        ship_of_entity_[instance.entity_id] = placed_ships_.size();
        placed_ships_.push_back(std::move(placed));
        land_session_records_.push_back(structures.at(instance.entity_id));
    }
}

namespace {
float motion_value(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / sim::math::Fixed::scale);
}
// D-8: replace this path-derived estimate when the simulation publishes the desired heading.
float land_heading_error(const platform::LiveLandMemberMotion& member) {
    if (!member.has_path_target) return 0.0F;
    const double dx = static_cast<double>(member.path_target.x.raw()) - static_cast<double>(member.position.x.raw());
    const double dy = static_cast<double>(member.path_target.y.raw()) - static_cast<double>(member.position.y.raw());
    if (dx == 0.0 && dy == 0.0) return 0.0F;
    constexpr double radians_to_degrees = 57.29577951308232;
    return static_cast<float>(std::remainder(std::atan2(dy, dx) * radians_to_degrees
        - static_cast<double>(motion_value(member.facing)) * 360.0, 360.0));
}
// The clip type each LandClip choice plays (WLAN-03..05, 10, 11); the numbers are the named clip types.
std::size_t land_clip_type(const animation::LandClip kind, const std::uint32_t stop_index) {
    namespace types = animation::clip_types;
    using enum animation::LandClip;
    switch (kind) {
    case move: return types::move; case walk: return types::walk_move; case crouch: return types::crouch_move;
    case start: return types::move_start;
    case stop: return types::move_end_first + std::min<std::size_t>(stop_index, types::move_end_last - types::move_end_first);
    case turn_left: return types::turn_left; case turn_right: return types::turn_right;
    case turn_left_quarter: return types::turn_left_quarter; case turn_right_quarter: return types::turn_right_quarter;
    case turn_left_half: return types::turn_left_half; case turn_right_half: return types::turn_right_half;
    case turn_left_begin: return types::turn_left_begin; case turn_left_end: return types::turn_left_end;
    case turn_right_begin: return types::turn_right_begin; case turn_right_end: return types::turn_right_end;
    case attack: return types::attack; case attack_idle: return types::attack_idle;
    default: return types::idle;
    }
}
} // namespace

void LiveSessionView::note_land_shots(const std::uint64_t tick) {
    const auto snapshot = snapshot_at(tick);
    if (!snapshot) return;
    for (const auto& event : snapshot->combat_events()) {
        if (event.kind != tactical::CombatEventKind::weapon_fired) continue;
        auto& latest = land_last_shot_[event.shooter];
        latest = std::max(latest, event.tick);
    }
}

void LiveSessionView::advance_land_playback(BattleScene& population, const platform::LiveLandMotion& motion) {
    constexpr std::uint32_t subdivisions = 1024;
    const auto authored = [](const auto& optional) { return optional ? motion_value(*optional) : 0.0F; };
    const auto constant = [&](const std::string_view name) {
        const auto found = std::find_if(tables_->land_animation_constants.begin(), tables_->land_animation_constants.end(),
            [&](const auto& row) { return row.tag == name; });
        return found == tables_->land_animation_constants.end() ? 0.0F : authored(found->value);
    };
    // N9: the constants do not change while the view lives; read them once per call, not once per member.
    const float walk_cutoff = constant("WalkAnimationCutoff"), move_blend = constant("MoveBlendTime"),
        crouch_blend = constant("CrouchMoveBlendTime"), team_move_blend = constant("TeamMoveBlendTime"),
        team_crouch_blend = constant("TeamCrouchMoveBlendTime"), infantry_turn_blend = constant("InfantryTurnBlendTime"),
        idle_walk_blend = constant("IdleWalkBlendTime"), crouch_idle_walk_blend = constant("CrouchIdleWalkBlendTime");
    for (const auto& member : motion.members) {
        const auto entry = land_playback_.find(member.entity);
        const auto ship = ship_of_entity_.find(member.entity);
        if (entry == land_playback_.end() || ship == ship_of_entity_.end()) continue;
        auto& playback = entry->second;
        const auto advance = [&](std::uint64_t position, const std::size_t type, const float rate, const bool loop) {
            const auto* player = population.land_clip(ship->second, type);
            if (!player || player->playable_frames() == 0) return std::uint64_t{};
            const auto end = static_cast<std::uint64_t>(player->playable_frames()) * subdivisions;
            const double amount = std::round(static_cast<double>(player->frames_per_second())
                * std::max(rate, 0.0F) * subdivisions / 30.0);
            if (!std::isfinite(amount) || amount < 0.0) return position;
            if (loop) {
                const auto increment = static_cast<std::uint64_t>(std::fmod(amount, static_cast<double>(end)));
                return (position % end + increment) % end;
            }
            if (position >= end || amount >= static_cast<double>(end - position)) return end;
            return position + static_cast<std::uint64_t>(amount);
        };
        if (playback.initialized) {
            playback.position = advance(playback.position, playback.type, playback.rate, playback.loop);
            if (playback.blending) playback.outgoing_position = advance(playback.outgoing_position,
                playback.outgoing_type, playback.outgoing_rate, playback.outgoing_loop);
        }
        const auto idle_position = population.land_idle_position(ship->second, motion.tick);
        const auto idle_units = [&]() -> std::uint64_t {
            if (!idle_position || idle_position->subdivisions == 0) return 0;
            return (idle_position->position / idle_position->subdivisions) * subdivisions
                + (idle_position->position % idle_position->subdivisions) * subdivisions / idle_position->subdivisions;
        };
        if (playback.type == 0 && idle_position) playback.position = idle_units();
        if (playback.blending && playback.outgoing_type == 0 && idle_position)
            playback.outgoing_position = idle_units();
        const auto* previous = population.land_clip(ship->second, playback.type);
        const bool finished = previous && playback.position >= static_cast<std::uint64_t>(previous->playable_frames()) * subdivisions;
        if (!playback.data.has_pre_turn_animation) playback.phase = animation::LandTurnPhase::main;
        else if (member.turning && !playback.turning) playback.phase = animation::LandTurnPhase::begin;
        else if (!member.turning && playback.turning) playback.phase = animation::LandTurnPhase::end;
        else if (finished && playback.phase == animation::LandTurnPhase::begin) playback.phase = animation::LandTurnPhase::main;
        else if (finished && playback.phase == animation::LandTurnPhase::end) playback.phase = animation::LandTurnPhase::main;
        animation::LandClipInputs input;
        switch (member.state) {
        case tactical::LandWalkState::starting: input.motion = animation::LandMotion::starting; break;
        case tactical::LandWalkState::moving: input.motion = animation::LandMotion::moving; break;
        case tactical::LandWalkState::turning: input.motion = animation::LandMotion::turning; break;
        case tactical::LandWalkState::stopping: input.motion = animation::LandMotion::stopping; break;
        default: input.motion = animation::LandMotion::stopped; break;
        }
        if (member.turning || playback.phase == animation::LandTurnPhase::end) input.motion = animation::LandMotion::turning;
        input.active = playback.active; input.turn_phase = playback.phase;
        input.speed = animation::land_clip_speed(motion_value(member.speed), motion_value(member.team_speed), member.has_team_speed);
        input.maximum_speed = motion_value(member.maximum_speed);
        if (!playback.bike && input.speed > 0.0F && input.motion == animation::LandMotion::turning)
            input.motion = animation::LandMotion::moving;
        input.heading_degrees = member.has_motion_input ? motion_value(member.heading_error) * 360.0F
            : land_heading_error(member);
        const float facing_delta = playback.initialized
            ? std::remainder(motion_value(member.facing) - playback.facing, 1.0F) : 0.0F;
        const float turn_rate_limit = motion_value(member.turn_rate_limit);
        input.turn_fraction = member.has_turn_fraction ? motion_value(member.turn_fraction)
            : animation::land_turn_fraction(facing_delta, turn_rate_limit, 30.0F);
        if (playback.bike) input.heading_degrees = facing_delta * 360.0F;
        input.move_speed = authored(playback.data.movement_animation_speed); input.walk_speed = authored(playback.data.walk_animation_speed);
        input.crouch_speed = authored(playback.data.crouch_animation_speed); input.rotation_speed = authored(playback.data.rotation_animation_speed);
        input.stopped_turn_rate = authored(playback.data.stopped_rate_of_turn); input.cutoff = walk_cutoff;
        input.move_blend = move_blend; input.crouch_blend = crouch_blend;
        input.squad_move_blend = team_move_blend; input.squad_crouch_blend = team_crouch_blend;
        input.infantry_turn_blend = infantry_turn_blend;
        input.turn_duration = turn_rate_limit > 0.0F
            ? std::abs(input.heading_degrees) / (360.0F * turn_rate_limit) : 0.0F;
        input.idle_walk_blend = idle_walk_blend; input.crouch_idle_walk_blend = crouch_idle_walk_blend;
        input.bike = playback.bike; input.team = playback.team; input.walk_transition = playback.data.walk_transition;
        input.pre_turn = playback.data.has_pre_turn_animation; input.spread_out = member.spread_out;
        input.stop_point = member.stop_point; input.blending = playback.blending;
        input.has_walk = population.land_clip(ship->second, animation::clip_types::walk_move) != nullptr;
        input.has_crouch = population.land_clip(ship->second, animation::clip_types::crouch_move) != nullptr;
        namespace types = animation::clip_types;
        constexpr std::array<std::size_t, 10> turn_types{types::turn_left, types::turn_right, types::turn_left_begin,
            types::turn_left_end, types::turn_right_begin, types::turn_right_end, types::turn_left_half, types::turn_left_quarter,
            types::turn_right_half, types::turn_right_quarter};
        input.has_turn = (member.has_path_target || input.turn_phase == animation::LandTurnPhase::end)
            && std::any_of(turn_types.begin(), turn_types.end(),
                [&](const auto type) { return population.land_clip(ship->second, type) != nullptr; });
        if (input.pre_turn && input.turn_phase == animation::LandTurnPhase::begin
            && !population.land_clip(ship->second, input.heading_degrees < 0 ? types::turn_left_begin : types::turn_right_begin)) {
            playback.phase = input.turn_phase = animation::LandTurnPhase::main;
        }
        if (input.pre_turn && input.turn_phase == animation::LandTurnPhase::end
            && !population.land_clip(ship->second, playback.active == animation::LandClip::turn_left
                || playback.active == animation::LandClip::turn_left_quarter || playback.active == animation::LandClip::turn_left_half
                || playback.active == animation::LandClip::turn_left_begin ? types::turn_left_end : types::turn_right_end)) {
            playback.phase = input.turn_phase = animation::LandTurnPhase::main;
            input.motion = animation::LandMotion::stopped;
        }
        for (std::size_t type = types::move_end_first; type <= types::move_end_last && population.land_clip(ship->second, type); ++type) ++input.stop_clip_count;
        if (previous && previous->playable_frames()) input.cycle_phase = static_cast<float>(playback.position)
            / static_cast<float>(static_cast<std::uint64_t>(previous->playable_frames()) * subdivisions);
        // UA-04 on land: the member's latest shot drives its ATTACK / ATTACKIDLE choice (presentation only; the model's
        // clips decide: none bound, no change).
        const auto shot = land_last_shot_.find(member.entity);
        const bool fresh_shot = shot != land_last_shot_.end() && shot->second != playback.shot_seen;
        input.ticks_since_shot = shot == land_last_shot_.end() ? animation::land_no_shot
            : fresh_shot || shot->second >= motion.tick ? 0U
            : static_cast<std::uint32_t>(std::min<std::uint64_t>(motion.tick - shot->second, animation::land_no_shot - 1));
        input.has_attack = population.land_clip(ship->second, types::attack) != nullptr;
        input.has_attack_idle = population.land_clip(ship->second, types::attack_idle) != nullptr;
        input.attack_finished = playback.active == animation::LandClip::attack && finished;
        input.attack_blend = 0.4F; // retail's global attack blend (its initial engine value, read from the debug build)
        // Retail draws the stance blend from the synchronized generator in [0.2, 0.75] s; a fixed spread by entity here.
        input.attack_idle_blend = 0.2F + 0.55F * static_cast<float>((member.entity * 2654435761ULL) % 1000) / 1000.0F;
        auto choice = animation::select_land_clip(input);
        auto selected = land_clip_type(choice.kind, choice.stop_index);
        if (!population.land_clip(ship->second, selected)) {
            selected = 0;
            choice = {};
        }
        const auto* player = population.land_clip(ship->second, selected);
        if (!player) continue;
        if (!playback.initialized || playback.type != selected) {
            playback.outgoing_type = playback.type; playback.outgoing_position = playback.position;
            playback.outgoing_rate = playback.rate; playback.outgoing_loop = playback.loop; playback.blend_start = motion.tick;
            playback.blend_ticks = animation::seconds_to_ticks(choice.blend_seconds, 30).value_or(0);
            playback.blending = playback.initialized && playback.blend_ticks != 0;
            const auto end = static_cast<std::uint64_t>(player->playable_frames()) * subdivisions;
            if (choice.start_frame == animation::LandStartFrame::zero) playback.position = 0;
            else if (choice.start_frame == animation::LandStartFrame::random)
                playback.position = end ? (member.entity * 2654435761ULL) % end : 0;
            else if (choice.start_frame == animation::LandStartFrame::normalized && previous && previous->playable_frames())
                playback.position = playback.position * player->playable_frames() / previous->playable_frames();
            if (end) playback.position %= end;
        } else if (choice.kind == animation::LandClip::attack && fresh_shot) {
            // A further shot restarts the ATTACK clip from its first frame.
            playback.position = 0; playback.blending = false;
        }
        if (shot != land_last_shot_.end()) playback.shot_seen = shot->second;
        if (selected == 0 && idle_position) playback.position = idle_units();
        if (choice.kind == animation::LandClip::start && selected == 16) {
            const auto end = static_cast<std::uint64_t>(player->playable_frames()) * subdivisions;
            const double position = static_cast<double>(std::max(0.0F, motion_value(member.transition_frame)))
                * player->frames_per_second() * choice.rate * subdivisions / 30.0;
            if (std::isfinite(position) && position >= 0.0)
                playback.position = position >= static_cast<double>(end) ? end : static_cast<std::uint64_t>(position);
        }
        playback.active = selected == 0 ? animation::LandClip::idle : choice.kind;
        playback.type = selected; playback.rate = choice.rate; playback.loop = choice.loop; playback.tick = motion.tick;
        playback.facing = motion_value(member.facing); playback.turning = member.turning; playback.initialized = true;
        if (playback.blending && motion.tick - playback.blend_start >= playback.blend_ticks) playback.blending = false;
        if (playback.bike) playback.roll = animation::land_bike_roll(playback.roll, authored(playback.data.max_rate_of_roll),
            authored(playback.data.bank_turn_angle), playback.active);
    }
}

} // namespace eawr::presentation::godot_backend
