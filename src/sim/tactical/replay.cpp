#include "replay_internal.hpp"

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

core::Result<void> validate_setup(const TacticalSetup& setup) {
    constexpr std::string_view path{};
    if (setup.match_policy && setup.match_policy->disabled_flags() == 0) {
        return fail<void>(diagnostic_codes::invalid_setup,
            "all-enabled match policy must use the legacy replay encoding", path);
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
        if (unit.purchase_type != 0 || !unit.contained.empty() || unit.purchase_token != 0) {
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
    if (replay.final_tick_count > max_ticks || replay.commands.size() > max_commands) {
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
    return core::Result<void>::success();
}


} // namespace eawr::sim::tactical
