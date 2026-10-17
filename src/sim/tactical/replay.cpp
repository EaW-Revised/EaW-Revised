#include "replay_internal.hpp"
#include <algorithm>
#include <functional>
#include <tuple>
#include <set>

namespace eawr::sim::tactical {
using namespace replay_detail;

namespace {
[[nodiscard]] const UnitState* setup_unit(const TacticalSetup& setup, const EntityId id) noexcept {
    const auto found = std::lower_bound(setup.units.begin(), setup.units.end(), id,
        [](const UnitState& unit, const EntityId value) { return unit.entity_id < value; });
    return found != setup.units.end() && found->entity_id == id ? &*found : nullptr;
}

// Squadron rules (#271): containers strictly increase, members strictly increase and are
// nonempty, every container and member is a setup unit of the container's owner, and no unit
// is listed twice (as a container or a member). Runs after the unit checks.
[[nodiscard]] core::Result<void> validate_squadrons(const TacticalSetup& setup) {
    constexpr std::string_view path{};
    if (setup.squadrons.size() > max_units) {
        return fail<void>(diagnostic_codes::resource_limit, "squadron count exceeds the unit limit", path);
    }
    std::vector<EntityId> listed;
    for (std::size_t index = 0; index < setup.squadrons.size(); ++index) {
        const auto& squadron = setup.squadrons[index];
        const auto label = "squadron " + std::to_string(squadron.container);
        if (index != 0 && squadron.container <= setup.squadrons[index - 1].container) {
            return fail<void>(diagnostic_codes::order,
                "squadron containers must strictly increase at index " + std::to_string(index), path);
        }
        const auto* container = setup_unit(setup, squadron.container);
        if (container == nullptr) {
            return fail<void>(diagnostic_codes::invalid_setup, label + ": the container is not a setup unit", path);
        }
        if (squadron.members.empty() || squadron.members.size() > max_units) {
            return fail<void>(diagnostic_codes::invalid_setup, label + ": a squadron lists 1 to 1,000,000 craft", path);
        }
        listed.push_back(squadron.container);
        for (std::size_t member = 0; member < squadron.members.size(); ++member) {
            const auto id = squadron.members[member];
            if (member != 0 && id <= squadron.members[member - 1]) {
                return fail<void>(diagnostic_codes::order, label + ": craft IDs must strictly increase", path);
            }
            const auto* craft = setup_unit(setup, id);
            if (craft == nullptr || craft->owner != container->owner) {
                return fail<void>(diagnostic_codes::invalid_setup,
                    label + ": craft " + std::to_string(id) + " is not a setup unit of the container's owner", path);
            }
            listed.push_back(id);
        }
    }
    std::sort(listed.begin(), listed.end());
    if (std::adjacent_find(listed.begin(), listed.end()) != listed.end()) {
        return fail<void>(diagnostic_codes::invalid_setup, "a unit belongs to more than one squadron", path);
    }
    return core::Result<void>::success();
}


} // namespace

void resolve_cinematic_seats(TacticalSetup& setup, const std::span<const EconomyPlayer> players) {
    for (const auto& player : setup.players) {
        const auto at = std::lower_bound(setup.cinematic_seats.begin(), setup.cinematic_seats.end(), player.player_id,
            [](const auto& row, const PlayerId id) { return row.player < id; });
        if (at != setup.cinematic_seats.end() && at->player == player.player_id) continue;
        const auto role = std::find_if(players.begin(), players.end(), [&](const auto& row) { return row.player == player.player_id; });
        setup.cinematic_seats.insert(at, {player.player_id, role == players.end() || !role->ai});
    }
}

core::Result<void> validate_setup(const TacticalSetup& setup) {
    if (setup.match_identity) {
        if (auto valid = sim::validate_match_identity(*setup.match_identity); !valid) return valid;
        const auto& domains = setup.match_identity->domains;
        const auto units = std::find_if(domains.begin(), domains.end(), [](const auto& domain) { return domain.name == "units"; });
        if (units == domains.end() || units->version != 1 || units->digest != setup.content_identity)
            return fail<void>(sim::match_identity_mismatch_code, "match identity mismatch: units", {});
        if (setup.skirmish) {
            for (const auto& expected : {sim::identity_domain("map-path", setup.skirmish->map),
                    sim::identity_domain("map-digest", setup.skirmish->map_sha256)}) {
                const auto actual = std::find_if(domains.begin(), domains.end(), [&](const auto& domain) { return domain.name == expected.name; });
                if (actual == domains.end() || *actual != expected)
                    return fail<void>(sim::match_identity_mismatch_code, "match identity mismatch: " + expected.name, {});
            }
        }
    }
    constexpr std::string_view path{};
    if (setup.first_free_entity_id && (*setup.first_free_entity_id == invalid_entity_id
        || std::any_of(setup.units.begin(), setup.units.end(), [&](const UnitState& unit) {
            return unit.entity_id >= *setup.first_free_entity_id;
        }))) {
        return fail<void>(diagnostic_codes::invalid_setup,
            "shared allocator must start above every setup entity ID", path);
    }
    if (setup.match_policy && setup.match_policy->disabled_flags() == 0) {
        return fail<void>(diagnostic_codes::invalid_setup,
            "all-enabled match policy must use the legacy replay encoding", path);
    }
    if (setup.attract_pool) {
        // MAT-15: a skirmish-mode demo battle, never a campaign battle; the type list is a strict set.
        const auto& types = setup.attract_pool->types;
        if (setup.campaign || (!types.empty() && types.front() == 0)
            || std::adjacent_find(types.begin(), types.end(), std::greater_equal<>{}) != types.end()) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "attract pool return needs strictly increasing nonzero types and no campaign binding", path);
        }
    }
    if (setup.campaign) {
        const auto invalid = [](const std::string& message) {
            return fail<void>(diagnostic_codes::invalid_setup, "campaign setup: " + message, {});
        };
        if (!setup.first_free_entity_id || setup.campaign->players.empty() || !setup.free_garrisons.empty())
            return invalid("requires a shared allocator, accounts and no free starting templates");
        EconomyRules accounts; accounts.campaign = true; accounts.players = setup.campaign->players;
        if (auto valid = validate_economy(accounts, setup.players); !valid) return valid;
        for (const auto& p : setup.campaign->players)
            if (!p.credit_cap || p.credits > *p.credit_cap) return invalid("account needs a bounded campaign credit cap");
        EntityId previous{};
        for (const auto& r : setup.campaign->reserves) {
            if (r.id <= previous || r.id >= *setup.first_free_entity_id || r.type == 0 || setup_unit(setup, r.id)
                || accounts.player(r.owner) == nullptr) return invalid("reserve identity, type or owner is invalid");
            previous = r.id;
        }
        if (!setup.campaign->reserve_order.empty()) {
            auto ids = setup.campaign->reserve_order;
            std::sort(ids.begin(), ids.end());
            if (ids.size() != setup.campaign->reserves.size()
                || !std::equal(ids.begin(), ids.end(), setup.campaign->reserves.begin(),
                    [](EntityId id, const CampaignReserve& reserve) { return id == reserve.id; }))
                return invalid("reserve order must name every reserve exactly once");
        }
        std::optional<std::tuple<PlayerId, EntityId, TypeId>> previous_upgrade;
        for (const auto& u : setup.campaign->upgrades) {
            const auto* holder = setup_unit(setup, u.holder);
            const auto key = std::tuple{u.owner, u.holder, u.type};
            if (u.type == 0 || !holder || (holder->owner != u.owner && !std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& owner) {
                    return owner.player_id == u.owner && std::any_of(setup.players.begin(), setup.players.end(), [&](const auto& ally) {
                        return ally.player_id == holder->owner && ally.team_id == owner.team_id;
                    });
                })) || !accounts.player(u.owner)
                || (previous_upgrade && key <= *previous_upgrade)) return invalid("upgrade holder or order is invalid");
            previous_upgrade = key;
        }
        EntityId previous_registered{};
        for (const auto id : setup.campaign->population_registered) {
            const auto* unit = setup_unit(setup, id);
            if (id <= previous_registered || !unit || !accounts.player(unit->owner))
                return invalid("registered population must name ascending initial force roots");
            previous_registered = id;
        }
        EntityId previous_leader{};
        for (const auto id : setup.campaign->victory_leaders) {
            const auto contains = [&](auto&& self, const CarriedObject& object, const std::size_t depth) -> bool {
                if (depth > 32) return false;
                return object.entity_id == id || std::any_of(object.members.begin(), object.members.end(),
                    [&](const auto& member) { return self(self, member, depth + 1); });
            };
            const bool known = std::any_of(setup.units.begin(), setup.units.end(), [&](const auto& unit) {
                return accounts.player(unit.owner) && (unit.entity_id == id || std::any_of(unit.contained.begin(), unit.contained.end(),
                    [&](const auto& object) { return contains(contains, object, 1); }));
            }) || std::any_of(setup.campaign->reserves.begin(), setup.campaign->reserves.end(),
                [&](const auto& reserve) { return reserve.id == id || std::any_of(reserve.contained.begin(), reserve.contained.end(),
                    [&](const auto& object) { return contains(contains, object, 1); }); });
            if (id <= previous_leader || !known)
                return invalid("victory leaders must name ascending initial force identities");
            previous_leader = id;
        }
        if (setup.campaign->victory_station != 0) {
            const auto* station = setup_unit(setup, setup.campaign->victory_station);
            if (!station || !accounts.player(station->owner)) return invalid("victory station is absent");
        }
        if (setup.campaign->corruption_price_multiplier.raw() <= 0)
            return invalid("campaign price multiplier must be positive");
    }
    if (setup.players.size() > max_players || setup.units.size() > max_units) {
        return fail<void>(diagnostic_codes::resource_limit,
            "player/unit count exceeds tactical replay resource limits", path);
    }
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        const auto& player = setup.players[index];
        if (player.player_id == 0
            || (index != 0 && player.player_id <= setup.players[index - 1].player_id)) {
            return fail<void>(diagnostic_codes::order,
                "player IDs must be nonzero and strictly increasing at index " + std::to_string(index),
                path);
        }
        if ((player.flags & ~player_flag_commandable) != 0U) {
            return fail<void>(diagnostic_codes::version,
                "unsupported player flags at index " + std::to_string(index), path);
        }
    }
    PlayerId previous_cinematic{};
    for (const auto& seat : setup.cinematic_seats) {
        if (seat.player <= previous_cinematic || std::none_of(setup.players.begin(), setup.players.end(),
            [&](const auto& player) { return player.player_id == seat.player; }))
            return fail<void>(diagnostic_codes::invalid_setup, "cinematic seats need ascending known player identities", path);
        previous_cinematic = seat.player;
    }
    if (setup.skirmish) {
        const auto& metadata = *setup.skirmish;
        const auto& match = metadata.match;
        const SkirmishMatchPolicy policy{match.allow_heroes, match.allow_superweapons,
            match.free_starting_units, match.pre_built_base};
        if (policy != setup.match_policy.value_or(SkirmishMatchPolicy{})
            || (metadata.victory_condition != 1U && metadata.victory_condition != 2U)) {
            return fail<void>(diagnostic_codes::invalid_setup, "SKSU match policy or victory condition is invalid", path);
        }
        std::size_t body_size = 68U;
        const auto text = [&](const std::string& value) {
            body_size += 2U + value.size();
            return value.size() <= 1024U && value.find('\0') == std::string::npos;
        };
        if (!text(metadata.map) || !text(metadata.map_sha256) || !text(match.win_condition)
            || !text(match.space_win_condition) || !metadata.map.starts_with("data/art/maps/")
            || !metadata.map.ends_with(".ted") || metadata.map.find("..") != std::string::npos
            || metadata.map.find('\\') != std::string::npos || metadata.map_sha256.size() != 64U
            || !std::all_of(metadata.map_sha256.begin(), metadata.map_sha256.end(),
                [](const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); })) {
            return fail<void>(diagnostic_codes::invalid_setup, "SKSU map or match text is invalid", path);
        }
        std::vector<PlayerId> commandable, recorded;
        for (const auto& player : setup.players) if (player.commandable()) commandable.push_back(player.player_id);
        for (const auto& slot : metadata.slots) {
            recorded.push_back(slot.player);
            body_size += 16U;
            if (slot.fleet.size() > 1024U || (slot.colour_index && *slot.colour_index >= max_players)) {
                return fail<void>(diagnostic_codes::invalid_setup, "SKSU fleet or palette exceeds limits", path);
            }
            for (const auto& type : slot.fleet) {
                if (!text(type) || type.empty()) {
                    return fail<void>(diagnostic_codes::invalid_setup, "SKSU fleet type is invalid", path);
                }
            }
        }
        if (recorded != commandable || body_size > 65000U) {
            return fail<void>(diagnostic_codes::invalid_setup, "SKSU slots or body length are invalid", path);
        }
    }
    for (std::size_t index = 0; index < setup.units.size(); ++index) {
        const auto& unit = setup.units[index];
        if (((unit.purchase_type != 0 || !unit.contained.empty()) && !setup.first_free_entity_id)
            || unit.purchase_token != 0) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "purchase identity and converted hero state must originate from a reinforcement command", path);
        }
        if (unit.entity_id == invalid_entity_id
            || (index != 0 && unit.entity_id <= setup.units[index - 1].entity_id)) {
            return fail<void>(diagnostic_codes::order,
                "unit IDs must be nonzero and strictly increasing at index " + std::to_string(index),
                path);
        }
        const auto owner = std::find_if(setup.players.begin(), setup.players.end(),
            [&unit](const Player& player) { return player.player_id == unit.owner; });
        if (owner == setup.players.end()) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "unit " + std::to_string(unit.entity_id) + " owner " + std::to_string(unit.owner)
                    + " is not a declared player",
                path);
        }
        if (!math::to_matrix(unit.rotation, unit.position)) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "unit " + std::to_string(unit.entity_id) + " rotation is not a unit quaternion",
                path);
        }
        if (unit.order != Order{} || unit.barrage_source != invalid_entity_id) {
            return fail<void>(diagnostic_codes::invalid_setup,
                "setup unit " + std::to_string(unit.entity_id) + " must not carry an order", path);
        }
    }
    const auto squadrons = validate_squadrons(setup);
    if (!squadrons) return squadrons;
    std::vector<EntityId> identities;
    for (const auto& unit : setup.units) identities.push_back(unit.entity_id);
    std::size_t nodes{};
    const auto carried_valid = [&](auto&& self, const CarriedObject& object, const EntityId parent,
                                   const std::size_t depth) -> bool {
        if (depth > 32 || ++nodes > max_units || object.entity_id == invalid_entity_id || object.type_id == 0
            || object.parent != parent || object.entity_id >= *setup.first_free_entity_id || !object.limbo
            || object.model_visible || object.collidable || object.selected || object.movement_coordinated) return false;
        identities.push_back(object.entity_id);
        for (const auto& member : object.members) if (!self(self, member, object.entity_id, depth + 1)) return false;
        return true;
    };
    for (const auto& unit : setup.units) for (const auto& object : unit.contained)
        if (!carried_valid(carried_valid, object, unit.entity_id, 1))
            return fail<void>(diagnostic_codes::invalid_setup, "invalid initial carried identity tree", path);
    if (setup.campaign) for (const auto& reserve : setup.campaign->reserves) {
        identities.push_back(reserve.id);
        for (const auto& object : reserve.contained)
            if (!carried_valid(carried_valid, object, reserve.id, 1))
                return fail<void>(diagnostic_codes::invalid_setup, "invalid reserve carried identity tree", path);
    }
    std::sort(identities.begin(), identities.end());
    if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
        return fail<void>(diagnostic_codes::invalid_setup, "initial identity is reused by a carrier or reserve", path);
    if (setup.departure_policy) {
        PlayerId previous{};
        for (const auto id : setup.departure_policy->humans) {
            const auto player = std::find_if(setup.players.begin(), setup.players.end(),
                [&](const Player& value) { return value.player_id == id; });
            if (id <= previous || player == setup.players.end() || !player->commandable())
                return fail<void>(diagnostic_codes::invalid_setup, "invalid multiplayer departure human roster", path);
            previous = id;
        }
    }
    PlayerId previous_beacon{};
    for (const auto& rule : setup.beacon_rules) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const Player& value) { return value.player_id == rule.player; });
        if (rule.player <= previous_beacon || player == setup.players.end() || !player->commandable()
            || rule.type == 0 || rule.lifetime_frames == 0 || rule.lifetime_frames > max_ticks)
            return fail<void>(diagnostic_codes::invalid_setup, "invalid beacon issuer, type or lifetime", path);
        previous_beacon = rule.player;
    }
    PlayerId previous_garrison{};
    for (const auto& binding : setup.free_garrisons) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const auto& value) { return value.player_id == binding.player; });
        if (binding.player <= previous_garrison || player == setup.players.end() || !player->commandable()
            || binding.templates.empty() || binding.templates.size() > 1024U
            || binding.registered.size() > max_units
            || !setup.match_policy.value_or(SkirmishMatchPolicy{}).free_starting_units) {
            return fail<void>(diagnostic_codes::invalid_setup, "invalid free garrison player or templates", path);
        }
        previous_garrison = binding.player;
        for (const auto type : binding.templates) if (type == 0)
            return fail<void>(diagnostic_codes::invalid_setup, "free garrison template type is zero", path);
        EntityId previous{};
        for (const auto id : binding.registered) {
            const auto unit = std::lower_bound(setup.units.begin(), setup.units.end(), id,
                [](const auto& value, const auto sought) { return value.entity_id < sought; });
            const auto team = std::lower_bound(setup.squadrons.begin(), setup.squadrons.end(), id,
                [](const auto& value, const auto sought) { return value.container < sought; });
            if (id <= previous || unit == setup.units.end() || unit->entity_id != id || unit->owner != binding.player
                || (team != setup.squadrons.end() && team->container == id
                    && (team->members.size() != 1 || team->members.front() != id))) {
                return fail<void>(diagnostic_codes::invalid_setup, "invalid free garrison registered object", path);
            }
            previous = id;
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_replay(const TacticalReplay& replay) {
    const auto setup = validate_setup(replay.setup);
    if (!setup) {
        return setup;
    }
    constexpr std::string_view path{};
    if (replay.setup.native_scene) {
        const auto& scene = *replay.setup.native_scene;
        if (!replay.setup.skirmish || !replay.setup.first_free_entity_id || scene.source_types.empty() || scene.battle_types.empty())
            return fail<void>(diagnostic_codes::invalid_setup, "native scene requires map, allocator and content roots", path);
        std::size_t size = 20U;
        for (const auto* names : {&scene.source_types, &scene.source_obstacles, &scene.battle_types}) {
            std::set<std::string> seen;
            for (const auto& name : *names) {
                size += 2U + name.size();
                auto normalized = name;
                for (auto& ch : normalized) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
                if (name.empty() || name.size() > std::numeric_limits<std::uint16_t>::max() || name.find('\0') != std::string::npos
                    || !seen.insert(normalized).second)
                    return fail<void>(diagnostic_codes::invalid_setup, "native scene type is empty, duplicate or oversized", path);
            }
        }
        if (size > std::numeric_limits<std::uint16_t>::max())
            return fail<void>(diagnostic_codes::invalid_setup, "native scene body exceeds header limit", path);
    }
    if (replay.final_tick_count > max_ticks || replay.commands.size() > max_commands
        || replay.object_turns.size() > max_commands) {
        return fail<void>(diagnostic_codes::resource_limit,
            "tick/command count exceeds tactical replay resource limits", path);
    }
    for (std::size_t index = 0; index < replay.commands.size(); ++index) {
        const auto& command = replay.commands[index];
        const auto label = command_label(index, command);
        if (command.key.tick >= replay.final_tick_count) {
            return fail<void>(diagnostic_codes::malformed,
                label + " tick is not less than final tick count", path);
        }
        if (index != 0 && !(replay.commands[index - 1].key < command.key)) {
            return fail<void>(diagnostic_codes::order,
                label + " does not strictly follow the previous (tick, player, sequence)", path);
        }
        const auto shape = detail::validate_command_shape(command, replay.setup.players, label, path);
        if (!shape) {
            return shape;
        }
        if (const auto* cancel = std::get_if<CancelPayload>(&command.payload);
            cancel != nullptr && cancel->entry_id != 0 && !replay.setup.queue_identities) {
            return fail<void>(diagnostic_codes::invalid_command, label + ": cancel entry requires QIDS extension", path);
        }
    }
    // #3644: object-turn uses are applied by the world at the seam, so a record names a unit and a real ability.
    for (std::size_t index = 0; index < replay.object_turns.size(); ++index) {
        const auto& record = replay.object_turns[index];
        const auto label = "object-turn record " + std::to_string(index);
        if (record.tick >= replay.final_tick_count) {
            return fail<void>(diagnostic_codes::malformed, label + " tick is not less than final tick count", path);
        }
        if (index != 0) {
            const auto& previous = replay.object_turns[index - 1];
            const std::tuple before{previous.tick, previous.use.unit, previous.use.sequence};
            if (!(before < std::tuple{record.tick, record.use.unit, record.use.sequence})) {
                return fail<void>(diagnostic_codes::order,
                    label + " does not strictly follow the previous (tick, unit, sequence)", path);
            }
        }
        if (record.use.unit == invalid_entity_id || record.use.ability == AbilityKind::none
            || static_cast<std::uint8_t>(record.use.ability) > static_cast<std::uint8_t>(AbilityKind::blast)
            || static_cast<std::uint8_t>(record.use.action) < static_cast<std::uint8_t>(AbilityAction::activate)
            || static_cast<std::uint8_t>(record.use.action) > static_cast<std::uint8_t>(AbilityAction::autofire_off)) {
            return fail<void>(diagnostic_codes::invalid_command, label + " names no unit, ability or action", path);
        }
    }
    return core::Result<void>::success();
}


} // namespace eawr::sim::tactical
