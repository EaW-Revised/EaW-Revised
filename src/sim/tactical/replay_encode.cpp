#include "replay_internal.hpp"


namespace eawr::sim::tactical {
using namespace replay_detail;

namespace {
[[nodiscard]] std::vector<std::uint8_t> skirmish_body(const ReplaySkirmishSetup& setup) {
    std::vector<std::uint8_t> bytes;
    const auto text = [&](const std::string& value) {
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    text(setup.map);
    text(setup.map_sha256);
    const auto& match = setup.match;
    const SkirmishMatchPolicy policy{match.allow_heroes, match.allow_superweapons,
        match.free_starting_units, match.pre_built_base};
    sim::detail::append_u32(bytes, policy.disabled_flags() | (match.allow_random_events ? 16U : 0U));
    sim::detail::append_i64(bytes, match.credits.raw());
    for (const auto value : {match.start_tech, match.max_tech, match.game_timer, match.win_integer, match.auto_resolve})
        sim::detail::append_i64(bytes, value);
    sim::detail::append_i64(bytes, match.win_float.raw());
    text(match.win_condition);
    text(match.space_win_condition);
    sim::detail::append_u32(bytes, setup.victory_condition);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(setup.slots.size()));
    for (const auto& slot : setup.slots) {
        sim::detail::append_u32(bytes, slot.player);
        sim::detail::append_u32(bytes, (slot.human ? 1U : 0U) | (static_cast<std::uint32_t>(slot.difficulty) << 1U));
        sim::detail::append_u32(bytes, slot.colour_index.value_or(std::numeric_limits<std::uint32_t>::max()));
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(slot.fleet.size()));
        for (const auto& type : slot.fleet) text(type);
    }
    return bytes;
}

// #3644 OBJT record, 64 bytes: tick, unit, sequence, ability, action, flags (bit 0: position), zero word,
// target, position (zero without the flag).
void append_object_turn(std::vector<std::uint8_t>& bytes, const ObjectTurnRecord& record) {
    const auto& use = record.use;
    sim::detail::append_u64(bytes, record.tick);
    sim::detail::append_u64(bytes, use.unit);
    sim::detail::append_u64(bytes, use.sequence);
    bytes.push_back(static_cast<std::uint8_t>(use.ability));
    bytes.push_back(static_cast<std::uint8_t>(use.action));
    sim::detail::append_u16(bytes, use.position ? 1U : 0U);
    sim::detail::append_u32(bytes, 0);
    sim::detail::append_u64(bytes, use.target);
    const math::Vec3 position = use.position.value_or(math::Vec3{});
    sim::detail::append_i64(bytes, position.x.raw());
    sim::detail::append_i64(bytes, position.y.raw());
    sim::detail::append_i64(bytes, position.z.raw());
}
} // namespace

std::optional<std::uint16_t> peek_replay_format_version(
    const std::span<const std::uint8_t> bytes) noexcept {
    if (bytes.size() < replay_magic.size() + 2
        || !std::equal(replay_magic.begin(), replay_magic.end(), bytes.begin())) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(
        bytes[8] | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[9]) << 8U));
}

core::Result<std::vector<std::uint8_t>> write_replay(const TacticalReplay& replay) {
    using Bytes = std::vector<std::uint8_t>;
    const auto validated = validate_replay(replay);
    if (!validated) {
        return core::Result<Bytes>::failure(validated.error());
    }
    const auto metadata = replay.setup.skirmish ? skirmish_body(*replay.setup.skirmish) : std::vector<std::uint8_t>{};
    Bytes native;
    if (replay.setup.native_scene) {
        const auto& scene = *replay.setup.native_scene;
        sim::detail::append_u32(native, 1);
        sim::detail::append_u32(native, scene.multiplayer ? 1U : 0U);
        for (const auto* names : {&scene.source_types, &scene.source_obstacles, &scene.battle_types}) {
            sim::detail::append_u32(native, static_cast<std::uint32_t>(names->size()));
            for (const auto& name : *names) {
                sim::detail::append_u16(native, static_cast<std::uint16_t>(name.size()));
                native.insert(native.end(), name.begin(), name.end());
            }
        }
    }
    Bytes campaign;
    Bytes carriers;
    const auto carrier_count = std::count_if(replay.setup.units.begin(), replay.setup.units.end(),
        [](const auto& unit) { return unit.purchase_type != 0 || !unit.contained.empty(); });
    const auto node = [&](auto&& self, Bytes& body, const CarriedObject& object) -> void {
        sim::detail::append_u64(body, object.entity_id);
        sim::detail::append_u64(body, object.type_id);
        sim::detail::append_u64(body, object.parent);
        sim::detail::append_u32(body, (object.named_hero ? 1U : 0U) | (object.generic_hero ? 2U : 0U)
            | (object.combat_preserved ? 4U : 0U));
        sim::detail::append_u32(body, static_cast<std::uint32_t>(object.members.size()));
        for (const auto& member : object.members) self(self, body, member);
    };
    if (carrier_count != 0) {
        sim::detail::append_u32(carriers, 1);
        sim::detail::append_u32(carriers, static_cast<std::uint32_t>(carrier_count));
        for (const auto& unit : replay.setup.units) if (unit.purchase_type != 0 || !unit.contained.empty()) {
            sim::detail::append_u64(carriers, unit.entity_id);
            sim::detail::append_u64(carriers, unit.purchase_type);
            sim::detail::append_u32(carriers, static_cast<std::uint32_t>(unit.contained.size()));
            for (const auto& object : unit.contained) node(node, carriers, object);
        }
    }
    if (replay.setup.campaign) {
        const auto& binding = *replay.setup.campaign;
        sim::detail::append_u32(campaign, binding.reserve_order.empty() ? 5U : 6U); // account build-time scale; optional source pool order
        sim::detail::append_i64(campaign, binding.corruption_price_multiplier.raw());
        sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.players.size()));
        for (const auto& p : binding.players) {
            sim::detail::append_u32(campaign, p.player);
            sim::detail::append_i64(campaign, p.credits.raw());
            sim::detail::append_u32(campaign, p.population_cap);
            sim::detail::append_u32(campaign, (p.ai ? 1U : 0U) | (p.strategic_credits ? 2U : 0U));
            sim::detail::append_i64(campaign, p.reinforcement_yaw.raw());
            sim::detail::append_u32(campaign, p.start_tech);
            sim::detail::append_u32(campaign, p.max_tech);
            sim::detail::append_i64(campaign, p.credit_multiplier.raw());
            sim::detail::append_i64(campaign, p.ai_build_time_multiplier.raw());
            sim::detail::append_i64(campaign, p.credit_cap->raw());
            sim::detail::append_i64(campaign, p.strategic_credits.value_or(math::Fixed{}).raw());
        }
        sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.reserves.size()));
        for (const auto& r : binding.reserves) {
            sim::detail::append_u64(campaign, r.id);
            sim::detail::append_u64(campaign, r.type);
            sim::detail::append_u32(campaign, r.owner);
            sim::detail::append_u32(campaign, r.restricted ? 1U : 0U);
            sim::detail::append_u64(campaign, r.purchase_type);
            sim::detail::append_u32(campaign, static_cast<std::uint32_t>(r.contained.size()));
            for (const auto& object : r.contained) node(node, campaign, object);
        }
        sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.upgrades.size()));
        for (const auto& u : binding.upgrades) {
            sim::detail::append_u64(campaign, u.type);
            sim::detail::append_u64(campaign, u.holder);
            sim::detail::append_u32(campaign, u.owner);
        }
        sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.population_registered.size()));
        for (const auto id : binding.population_registered) sim::detail::append_u64(campaign, id);
        sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.victory_leaders.size()));
        for (const auto id : binding.victory_leaders) sim::detail::append_u64(campaign, id);
        sim::detail::append_u64(campaign, binding.victory_station);
        if (!binding.reserve_order.empty()) {
            sim::detail::append_u32(campaign, static_cast<std::uint32_t>(binding.reserve_order.size()));
            for (const auto id : binding.reserve_order) sim::detail::append_u64(campaign, id);
        }
    }
    auto identity = replay.setup.match_identity ? sim::canonical_match_identity(*replay.setup.match_identity)
        : std::vector<std::uint8_t>{};
    if (replay.setup.match_identity) identity.insert(identity.end(),
        replay.setup.match_identity->sha256.begin(), replay.setup.match_identity->sha256.end());
    std::size_t garrison_size = replay.setup.free_garrisons.empty() ? 0U : 8U;
    for (const auto& binding : replay.setup.free_garrisons)
        garrison_size += 16U + 8U * (binding.templates.size() + binding.registered.size());
    std::vector<EntityId> disabled_garrisons;
    for (const auto& unit : replay.setup.units) {
        if (!unit.garrison_enabled) disabled_garrisons.push_back(unit.entity_id);
    }
    const bool extensions = replay.setup.match_policy.has_value() || replay.setup.skirmish.has_value()
        || !disabled_garrisons.empty() || replay.setup.queue_identities || garrison_size != 0
        || replay.setup.first_free_entity_id.has_value() || replay.setup.campaign.has_value() || !carriers.empty()
        || !replay.setup.cinematic_seats.empty() || !identity.empty()
        || replay.setup.departure_policy.has_value() || !replay.setup.beacon_rules.empty()
        || replay.setup.attract_pool.has_value() || replay.setup.native_scene.has_value()
        || !replay.object_turns.empty();
    const auto header_size = replay_header_size + (extensions ? 4U : 0U)
        + (replay.setup.match_policy ? 8U : 0U) + (replay.setup.skirmish ? 4U + metadata.size() : 0U)
        + (disabled_garrisons.empty() ? 0U : 4U + 8U * disabled_garrisons.size())
        + (replay.setup.queue_identities ? 8U : 0U) + (garrison_size == 0 ? 0U : 4U + garrison_size)
        + (replay.setup.first_free_entity_id ? 12U : 0U) + (campaign.empty() ? 0U : 4U + campaign.size())
        + (carriers.empty() ? 0U : 4U + carriers.size())
        + (replay.setup.cinematic_seats.empty() ? 0U : 12U + 8U * replay.setup.cinematic_seats.size())
        + (identity.empty() ? 0U : 4U + identity.size())
        + (replay.setup.departure_policy ? 12U + 4U * replay.setup.departure_policy->humans.size() : 0U)
        + (replay.setup.beacon_rules.empty() ? 0U : 12U + 16U * replay.setup.beacon_rules.size())
        + (replay.setup.attract_pool ? 12U + 8U * replay.setup.attract_pool->types.size() : 0U)
        + (native.empty() ? 0U : 4U + native.size())
        + (replay.object_turns.empty() ? 0U : 12U);
    if (header_size > std::numeric_limits<std::uint16_t>::max()) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "encoded replay header exceeds the 65535-byte limit", {});
    }
    std::size_t size = header_size + replay.setup.players.size() * detail::player_record_size
        + replay.setup.units.size() * detail::unit_record_size;
    const auto& squadrons = replay.setup.squadrons;
    for (const auto& squadron : squadrons) {
        size += 16U + 8U * squadron.members.size();
    }
    if (size > replay_max_bytes) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "encoded replay exceeds the 256 MiB output limit", {});
    }
    if (replay.object_turns.size() > max_commands
        || size > replay_max_bytes - replay.object_turns.size() * replay_object_turn_record_size) {
        return fail<Bytes>(diagnostic_codes::resource_limit, "encoded replay exceeds the 256 MiB output limit", {});
    }
    size += replay.object_turns.size() * replay_object_turn_record_size;
    for (const auto& command : replay.commands) {
        const auto record = 4 + detail::command_body_size(command);
        if (size > replay_max_bytes - record) {
            return fail<Bytes>(diagnostic_codes::resource_limit,
                "encoded replay exceeds the 256 MiB output limit", {});
        }
        size += record;
    }
    Bytes bytes;
    bytes.reserve(size);
    bytes.insert(bytes.end(), replay_magic.begin(), replay_magic.end());
    const auto format = extensions
        ? (squadrons.empty() ? replay_format_version_extensions : replay_format_version_squadron_extensions)
        : (squadrons.empty() ? replay_format_version : replay_format_version_squadrons);
    sim::detail::append_u16(bytes, format);
    sim::detail::append_u16(bytes, static_cast<std::uint16_t>(header_size));
    sim::detail::append_u32(bytes, tactical_rules_version);
    sim::detail::append_u32(bytes, math_version);
    sim::detail::append_u32(bytes, fractional_bits);
    sim::detail::append_u32(bytes, tick_numerator);
    sim::detail::append_u32(bytes, tick_denominator);
    sim::detail::append_u64(bytes, replay.setup.seed);
    sim::detail::append_u64(bytes, replay.final_tick_count);
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.players.size()));
    sim::detail::append_u32(bytes, static_cast<std::uint32_t>(squadrons.size()));
    sim::detail::append_u64(bytes, replay.setup.units.size());
    sim::detail::append_u64(bytes, replay.commands.size());
    bytes.insert(bytes.end(), replay.setup.content_identity.begin(), replay.setup.content_identity.end());
    if (extensions) {
        sim::detail::append_u32(bytes, (replay.setup.match_policy ? 1U : 0U) + (replay.setup.skirmish ? 1U : 0U)
            + (disabled_garrisons.empty() ? 0U : 1U) + (replay.setup.queue_identities ? 1U : 0U)
            + (garrison_size == 0 ? 0U : 1U) + (replay.setup.first_free_entity_id ? 1U : 0U)
            + (replay.setup.campaign ? 1U : 0U) + (carriers.empty() ? 0U : 1U)
            + (replay.setup.cinematic_seats.empty() ? 0U : 1U)
            + (identity.empty() ? 0U : 1U) + (replay.setup.departure_policy ? 1U : 0U)
            + (replay.setup.beacon_rules.empty() ? 0U : 1U) + (replay.setup.attract_pool ? 1U : 0U) + (replay.setup.native_scene ? 1U : 0U)
            + (replay.object_turns.empty() ? 0U : 1U));
    }
    if (replay.setup.match_policy) {
        sim::detail::append_u16(bytes, replay_extension_match_policy);
        sim::detail::append_u16(bytes, 4); // body length, excluding tag/length
        sim::detail::append_u32(bytes, replay.setup.match_policy->disabled_flags());
    }
    if (replay.setup.skirmish) {
        sim::detail::append_u16(bytes, replay_extension_skirmish_setup);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(metadata.size()));
        bytes.insert(bytes.end(), metadata.begin(), metadata.end());
    }
    if (!disabled_garrisons.empty()) {
        sim::detail::append_u16(bytes, replay_extension_garrison_disabled);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(8U * disabled_garrisons.size()));
        for (const auto id : disabled_garrisons) sim::detail::append_u64(bytes, id);
    }
    if (replay.setup.queue_identities) {
        sim::detail::append_u16(bytes, replay_extension_queue_identities);
        sim::detail::append_u16(bytes, 4);
        sim::detail::append_u32(bytes, 1); // QIDS extension version
    }
    if (garrison_size != 0) {
        sim::detail::append_u16(bytes, replay_extension_free_garrison);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(garrison_size));
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.free_garrisons.size()));
        for (const auto& binding : replay.setup.free_garrisons) {
            sim::detail::append_u32(bytes, binding.player);
            sim::detail::append_u32(bytes, binding.delay_frames);
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(binding.templates.size()));
            sim::detail::append_u32(bytes, static_cast<std::uint32_t>(binding.registered.size()));
            for (const auto type : binding.templates) sim::detail::append_u64(bytes, type);
            for (const auto id : binding.registered) sim::detail::append_u64(bytes, id);
        }
    }
    if (replay.setup.first_free_entity_id) {
        sim::detail::append_u16(bytes, replay_extension_first_free_id);
        sim::detail::append_u16(bytes, 8);
        sim::detail::append_u64(bytes, *replay.setup.first_free_entity_id);
    }
    if (!identity.empty()) {
        sim::detail::append_u16(bytes, replay_extension_match_identity);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(identity.size()));
        bytes.insert(bytes.end(), identity.begin(), identity.end());
    }
    if (replay.setup.departure_policy) {
        sim::detail::append_u16(bytes, replay_extension_multiplayer_departure);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(8U + 4U * replay.setup.departure_policy->humans.size()));
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.departure_policy->humans.size()));
        for (const auto id : replay.setup.departure_policy->humans) sim::detail::append_u32(bytes, id);
    }
    if (!replay.setup.beacon_rules.empty()) {
        sim::detail::append_u16(bytes, replay_extension_beacon_rules);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(8U + 16U * replay.setup.beacon_rules.size()));
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.beacon_rules.size()));
        for (const auto& rule : replay.setup.beacon_rules) {
            sim::detail::append_u32(bytes, rule.player);
            sim::detail::append_u32(bytes, rule.lifetime_frames);
            sim::detail::append_u64(bytes, rule.type);
        }
    }
    if (!campaign.empty()) {
        sim::detail::append_u16(bytes, replay_extension_campaign);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(campaign.size()));
        bytes.insert(bytes.end(), campaign.begin(), campaign.end());
    }
    if (!carriers.empty()) {
        sim::detail::append_u16(bytes, replay_extension_carriers);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(carriers.size()));
        bytes.insert(bytes.end(), carriers.begin(), carriers.end());
    }
    if (!replay.setup.cinematic_seats.empty()) {
        sim::detail::append_u16(bytes, replay_extension_cinematic_seats);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(8U + 8U * replay.setup.cinematic_seats.size()));
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.cinematic_seats.size()));
        for (const auto& seat : replay.setup.cinematic_seats) {
            sim::detail::append_u32(bytes, seat.player);
            sim::detail::append_u32(bytes, seat.cinematics_enabled ? 1U : 0U);
        }
    }
    if (replay.setup.attract_pool) {
        sim::detail::append_u16(bytes, replay_extension_attract_pool);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(8U + 8U * replay.setup.attract_pool->types.size()));
        sim::detail::append_u32(bytes, 1);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.setup.attract_pool->types.size()));
        for (const auto type : replay.setup.attract_pool->types) sim::detail::append_u64(bytes, type);
    }
    if (!native.empty()) {
        sim::detail::append_u16(bytes, replay_extension_native_scene);
        sim::detail::append_u16(bytes, static_cast<std::uint16_t>(native.size()));
        bytes.insert(bytes.end(), native.begin(), native.end());
    }
    if (!replay.object_turns.empty()) {
        sim::detail::append_u16(bytes, replay_extension_object_turn);
        sim::detail::append_u16(bytes, 8);
        sim::detail::append_u32(bytes, 1); // OBJT extension version
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(replay.object_turns.size()));
    }
    for (const auto& player : replay.setup.players) {
        detail::append_player(bytes, player);
    }
    for (const auto& unit : replay.setup.units) {
        detail::append_unit_record(bytes, unit);
    }
    for (const auto& squadron : squadrons) {
        sim::detail::append_u64(bytes, squadron.container);
        sim::detail::append_u32(bytes, static_cast<std::uint32_t>(squadron.members.size()));
        sim::detail::append_u32(bytes, 0);
        for (const auto member : squadron.members) {
            sim::detail::append_u64(bytes, member);
        }
    }
    for (const auto& command : replay.commands) {
        detail::append_command(bytes, command);
    }
    for (const auto& record : replay.object_turns) append_object_turn(bytes, record);
    return core::Result<Bytes>::success(std::move(bytes));
}

} // namespace eawr::sim::tactical
