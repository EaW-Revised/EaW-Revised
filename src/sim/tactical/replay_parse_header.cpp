#include "replay_parse_internal.hpp"

namespace eawr::sim::tactical::replay_parse_detail {
using namespace replay_detail;

namespace {
[[nodiscard]] bool read_skirmish(Reader& reader, ReplaySkirmishSetup& setup) {
    const auto text = [&](std::string& value) {
        std::uint16_t length{};
        if (!reader.read_u16(length) || length > 1024U || length > reader.remaining()) return false;
        value.resize(length);
        return reader.read_bytes(std::span(reinterpret_cast<std::uint8_t*>(value.data()), value.size()));
    };
    std::uint32_t flags{}, slots{};
    std::int64_t credits{}, win_float{};
    auto& match = setup.match;
    if (!text(setup.map) || !text(setup.map_sha256) || !reader.read_u32(flags) || (flags & ~31U) != 0
        || !reader.read_i64(credits)) return false;
    for (auto* value : {&match.start_tech, &match.max_tech, &match.game_timer, &match.win_integer, &match.auto_resolve}) {
        std::int64_t decoded{};
        if (!reader.read_i64(decoded) || decoded < std::numeric_limits<std::int32_t>::min()
            || decoded > std::numeric_limits<std::int32_t>::max()) return false;
        *value = static_cast<std::int32_t>(decoded);
    }
    if (!reader.read_i64(win_float) || !text(match.win_condition) || !text(match.tactical_win_condition)
        || !reader.read_u32(setup.victory_condition) || !reader.read_u32(slots) || slots > max_players) return false;
    match.allow_heroes = (flags & 1U) == 0;
    match.allow_superweapons = (flags & 2U) == 0;
    match.free_starting_units = (flags & 4U) == 0;
    match.pre_built_base = (flags & 8U) == 0;
    match.allow_random_events = (flags & 16U) != 0;
    match.credits = math::Fixed::from_raw(credits);
    match.win_float = math::Fixed::from_raw(win_float);
    for (std::uint32_t index = 0; index < slots; ++index) {
        ReplayLobbySlot slot;
        std::uint32_t human{}, colour{}, fleet{};
        if (!reader.read_u32(slot.player) || !reader.read_u32(human) || human > 4U || ((human & 1U) != 0 && human != 1U)
            || !reader.read_u32(colour) || !reader.read_u32(fleet) || fleet > 1024U) return false;
        slot.human = (human & 1U) != 0;
        slot.difficulty = static_cast<AiLevel>(human >> 1U);
        if (colour != std::numeric_limits<std::uint32_t>::max()) slot.colour_index = colour;
        for (std::uint32_t type = 0; type < fleet; ++type) {
            std::string name;
            if (!text(name)) return false;
            slot.fleet.push_back(std::move(name));
        }
        setup.slots.push_back(std::move(slot));
    }
    return reader.remaining() == 0;
}

[[nodiscard]] bool read_carried_object(Reader& body, std::size_t& nodes, CarriedObject& object, const std::size_t depth) {
    std::uint32_t flags{}, members{};
    if (depth > 32 || ++nodes > max_units || !body.read_u64(object.entity_id)
        || !body.read_u64(object.type_id) || !body.read_u64(object.parent)
        || !body.read_u32(flags) || (flags & ~7U) != 0 || !body.read_u32(members)
        || members > body.remaining() / 32U) return false;
    object.named_hero = (flags & 1U) != 0;
    object.generic_hero = (flags & 2U) != 0;
    object.combat_preserved = (flags & 4U) != 0;
    object.members.resize(members);
    for (auto& member : object.members) if (!read_carried_object(body, nodes, member, depth + 1)) return false;
    return true;
}

} // namespace

core::Result<void> parse_fixed_header(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    std::uint32_t& header_size,
    std::uint32_t& header_reserved,
    std::uint32_t& player_count,
    std::uint64_t& unit_count,
    std::uint64_t& command_count,
    bool& squadrons,
    bool& extensions) {
    using Parsed = void;
    std::array<std::uint8_t, 8> magic{};
    std::uint16_t format{};
    std::uint32_t rules{};
    std::uint32_t decoded_math{};
    std::uint32_t decoded_bits{};
    std::uint32_t numerator{};
    std::uint32_t denominator{};
    if (!reader.read_bytes(magic) || !reader.read_u16(format))
        return fail<Parsed>(diagnostic_codes::malformed, "truncated replay format", logical_path);
    const bool wide_header = format == replay_format_version_native || format == replay_format_version_squadron_native;
    std::uint16_t narrow_header_size{};
    const bool length_read = wide_header ? reader.read_u32(header_size) : reader.read_u16(narrow_header_size);
    if (!wide_header) header_size = narrow_header_size;
    if (!length_read
        || !reader.read_u32(rules) || !reader.read_u32(decoded_math) || !reader.read_u32(decoded_bits)
        || !reader.read_u32(numerator) || !reader.read_u32(denominator)
        || !reader.read_u64(replay.setup.seed) || !reader.read_u64(replay.final_tick_count)
        || !reader.read_u32(player_count) || !reader.read_u32(header_reserved)
        || !reader.read_u64(unit_count) || !reader.read_u64(command_count)
        || !reader.read_bytes(replay.setup.content_identity)) {
        return fail<Parsed>(diagnostic_codes::malformed, "truncated replay-v2 header", logical_path);
    }
    if (magic != replay_magic) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid replay magic", logical_path);
    }
    extensions = wide_header || format == replay_format_version_extensions
        || format == replay_format_version_squadron_extensions;
    squadrons = format == replay_format_version_squadron_native || format == replay_format_version_squadrons
        || format == replay_format_version_squadron_extensions;
    if ((format != replay_format_version && !squadrons && !extensions) || rules != tactical_rules_version
        || decoded_math != math_version || decoded_bits != fractional_bits) {
        return fail<Parsed>(diagnostic_codes::version,
            "unsupported replay-v2 contract versions (format=" + std::to_string(format)
                + ", rules=" + std::to_string(rules) + ", math=" + std::to_string(decoded_math)
                + ", fractional_bits=" + std::to_string(decoded_bits) + ")",
            logical_path);
    }
    if ((!extensions && header_size != replay_header_size)
        || (extensions && (header_size < replay_header_size + (wide_header ? 6U : 4U) || header_size > bytes.size()))) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "invalid replay-v2 header size " + std::to_string(header_size), logical_path);
    }
    if (numerator != tick_numerator || denominator != tick_denominator) {
        return fail<Parsed>(diagnostic_codes::version,
            "tactical rules v1 require a 1/30 s tick, found " + std::to_string(numerator) + "/"
                + std::to_string(denominator),
            logical_path);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_extension_cinematic_seats(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    Reader body(bytes.subspan(reader.offset(), length));
    std::uint32_t version{}, seats{};
    if (!body.read_u32(version) || version != 1 || !body.read_u32(seats)
        || seats == 0 || seats > max_players || length != 8U + 8U * seats)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid cinematic seat extension", logical_path);
    for (std::uint32_t seat = 0; seat < seats; ++seat) {
        CinematicSeat row; std::uint32_t enabled{};
        if (!body.read_u32(row.player) || !body.read_u32(enabled) || enabled > 1)
            return fail<Parsed>(diagnostic_codes::malformed, "invalid cinematic seat setting", logical_path);
        row.cinematics_enabled = enabled != 0;
        replay.setup.cinematic_seats.push_back(row);
    }
    std::vector<std::uint8_t> consumed(length);
    static_cast<void>(reader.read_bytes(consumed));
    return core::Result<void>::success();
}

core::Result<void> parse_extension_match_identity(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    Reader body(bytes.subspan(reader.offset(), length));
    constexpr std::string_view domain = "eawr-match-identity";
    std::vector<std::uint8_t> prefix(domain.size());
    sim::MatchIdentity identity;
    if (!body.read_bytes(prefix) || !std::equal(prefix.begin(), prefix.end(), domain.begin())
        || !body.read_u32(identity.version))
        return fail<Parsed>(sim::match_identity_invalid_code, "invalid MTID header", logical_path);
    std::uint32_t count_domains{};
    if (!body.read_u32(count_domains) || count_domains == 0 || count_domains > 64)
        return fail<Parsed>(sim::match_identity_invalid_code, "invalid MTID domain count", logical_path);
    for (std::uint32_t domain_index = 0; domain_index < count_domains; ++domain_index) {
        sim::IdentityDomain item;
        std::uint32_t size{};
        if (!body.read_u32(size) || size == 0 || size > 128 || size > body.remaining())
            return fail<Parsed>(sim::match_identity_invalid_code, "invalid MTID component length", logical_path);
        item.name.resize(size);
        if (!body.read_bytes(std::span(reinterpret_cast<std::uint8_t*>(item.name.data()), item.name.size()))
            || !body.read_u32(item.version) || !body.read_bytes(item.digest))
            return fail<Parsed>(sim::match_identity_invalid_code, "truncated MTID component", logical_path);
        identity.domains.push_back(std::move(item));
    }
    if (!body.read_bytes(identity.sha256) || body.remaining() != 0)
        return fail<Parsed>(sim::match_identity_invalid_code, "invalid MTID digest length", logical_path);
    if (auto valid = sim::validate_match_identity(identity); !valid)
        return core::Result<Parsed>::failure(valid.error());
    replay.setup.match_identity = std::move(identity);
    std::vector<std::uint8_t> consumed(length);
    static_cast<void>(reader.read_bytes(consumed));
    return core::Result<void>::success();
}

core::Result<void> parse_extension_carriers(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    std::vector<UnitState>& carriers,
    const std::uint16_t length) {
    using Parsed = void;
    Reader body(bytes.subspan(reader.offset(), length));
    std::uint32_t version{}, count_carriers{};
    if (!body.read_u32(version) || version != 1 || !body.read_u32(count_carriers)
        || count_carriers == 0 || count_carriers > body.remaining() / 20U)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid carrier header", logical_path);
    std::size_t nodes{};
    EntityId previous{};
    for (std::uint32_t row = 0; row < count_carriers; ++row) {
        UnitState carrier; std::uint32_t roots{};
        if (!body.read_u64(carrier.entity_id) || carrier.entity_id <= previous
            || !body.read_u64(carrier.purchase_type) || !body.read_u32(roots)
            || roots > body.remaining() / 32U || (roots == 0 && carrier.purchase_type == 0))
            return fail<Parsed>(diagnostic_codes::malformed, "invalid carrier row", logical_path);
        previous = carrier.entity_id;
        carrier.contained.resize(roots);
        for (auto& object : carrier.contained) if (!read_carried_object(body, nodes, object, 1))
            return fail<Parsed>(diagnostic_codes::malformed, "invalid carried tree", logical_path);
        carriers.push_back(std::move(carrier));
    }
    if (body.remaining() != 0)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid carrier body length", logical_path);
    std::vector<std::uint8_t> consumed(length);
    static_cast<void>(reader.read_bytes(consumed));
    return core::Result<void>::success();
}

core::Result<void> parse_extension_campaign(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    Reader body(bytes.subspan(reader.offset(), length));
    CampaignBattleSetup binding;
    std::uint32_t version{}, count_players{}, count_reserves{}, count_upgrades{};
    std::int64_t price_multiplier{};
    if (!body.read_u32(version) || (version != 5 && version != 6) || !body.read_i64(price_multiplier) || !body.read_u32(count_players)
        || count_players == 0 || count_players > max_players)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign setup header", logical_path);
    binding.corruption_price_multiplier = math::Fixed::from_raw(price_multiplier);
    for (std::uint32_t row = 0; row < count_players; ++row) {
        EconomyPlayer p;
        std::uint32_t flags{};
        std::int64_t credits{}, yaw{}, multiplier{}, build_multiplier{}, cap{}, strategic{};
        if (!body.read_u32(p.player) || !body.read_i64(credits) || !body.read_u32(p.population_cap)
            || !body.read_u32(flags) || flags > 3 || !body.read_i64(yaw)
            || !body.read_u32(p.start_tech) || !body.read_u32(p.max_tech)
            || !body.read_i64(multiplier) || !body.read_i64(build_multiplier) || !body.read_i64(cap) || !body.read_i64(strategic)
            || ((flags & 2U) == 0 && strategic != 0))
            return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign account", logical_path);
        p.credits = math::Fixed::from_raw(credits);
        p.ai = (flags & 1U) != 0;
        p.reinforcement_yaw = math::Fixed::from_raw(yaw);
        p.credit_multiplier = math::Fixed::from_raw(multiplier);
        p.ai_build_time_multiplier = math::Fixed::from_raw(build_multiplier);
        p.credit_cap = math::Fixed::from_raw(cap);
        if ((flags & 2U) != 0) p.strategic_credits = math::Fixed::from_raw(strategic);
        binding.players.push_back(p);
    }
    if (!body.read_u32(count_reserves) || count_reserves > body.remaining() / 36U)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign reserve count", logical_path);
    std::size_t reserve_nodes{};
    for (std::uint32_t row = 0; row < count_reserves; ++row) {
        CampaignReserve r; std::uint32_t restricted{}, roots{};
        if (!body.read_u64(r.id) || !body.read_u64(r.type) || !body.read_u32(r.owner)
            || !body.read_u32(restricted) || restricted > 1 || !body.read_u64(r.purchase_type)
            || !body.read_u32(roots) || roots > body.remaining() / 32U)
            return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign reserve", logical_path);
        r.restricted = restricted != 0;
        r.contained.resize(roots);
        for (auto& object : r.contained) if (!read_carried_object(body, reserve_nodes, object, 1))
            return fail<Parsed>(diagnostic_codes::malformed, "invalid reserve attachment tree", logical_path);
        binding.reserves.push_back(r);
    }
    if (!body.read_u32(count_upgrades) || count_upgrades > body.remaining() / 20U)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign upgrade count", logical_path);
    for (std::uint32_t row = 0; row < count_upgrades; ++row) {
        CampaignUpgrade u;
        if (!body.read_u64(u.type) || !body.read_u64(u.holder) || !body.read_u32(u.owner))
            return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign upgrade", logical_path);
        binding.upgrades.push_back(u);
    }
    std::uint32_t registered{};
    if (!body.read_u32(registered) || registered > body.remaining() / 8U)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid registered population count", logical_path);
    binding.population_registered.resize(registered);
    for (auto& id : binding.population_registered) if (!body.read_u64(id))
        return fail<Parsed>(diagnostic_codes::malformed, "truncated registered population", logical_path);
    std::uint32_t leaders{};
    if (!body.read_u32(leaders) || leaders > body.remaining() / 8U)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign leader count", logical_path);
    binding.victory_leaders.resize(leaders);
    for (auto& id : binding.victory_leaders) if (!body.read_u64(id))
        return fail<Parsed>(diagnostic_codes::malformed, "truncated campaign leader", logical_path);
    if (!body.read_u64(binding.victory_station))
        return fail<Parsed>(diagnostic_codes::malformed, "truncated campaign station condition", logical_path);
    if (version == 6) {
        std::uint32_t count_ordered_reserves{};
        if (!body.read_u32(count_ordered_reserves) || count_ordered_reserves == 0
            || count_ordered_reserves != count_reserves || count_ordered_reserves > body.remaining() / 8U)
            return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign reserve order count", logical_path);
        binding.reserve_order.resize(count_ordered_reserves);
        for (auto& id : binding.reserve_order) if (!body.read_u64(id))
            return fail<Parsed>(diagnostic_codes::malformed, "truncated campaign reserve order", logical_path);
    }
    if (body.remaining() != 0)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid campaign setup length", logical_path);
    std::vector<std::uint8_t> consumed(length);
    static_cast<void>(reader.read_bytes(consumed));
    replay.setup.campaign = std::move(binding);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_first_free_id(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    EntityId first{};
    if (length != 8 || !reader.read_u64(first) || first == invalid_entity_id) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid shared allocator extension", logical_path);
    }
    replay.setup.first_free_entity_id = first;
    return core::Result<void>::success();
}

core::Result<void> parse_extension_multiplayer_departure(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    std::uint32_t version{}, humans{};
    if (length < 8 || !reader.read_u32(version) || !reader.read_u32(humans)
        || version != 1 || humans > max_players || length != 8U + 4U * humans)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid multiplayer departure extension", logical_path);
    replay.setup.departure_policy.emplace();
    for (std::uint32_t human = 0; human < humans; ++human) {
        PlayerId id{};
        if (!reader.read_u32(id)) return fail<Parsed>(diagnostic_codes::malformed, "truncated departure roster", logical_path);
        replay.setup.departure_policy->humans.push_back(id);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_extension_free_garrison(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    const auto end = reader.offset() + length;
    std::uint32_t version{}, players{};
    if (length < 8 || !reader.read_u32(version) || !reader.read_u32(players)
        || version != 1 || players == 0 || players > max_players) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid GARR extension header", logical_path);
    }
    for (std::uint32_t row = 0; row < players; ++row) {
        FreeGarrisonSetup binding;
        std::uint32_t types{}, objects{};
        if (end - reader.offset() < 16 || !reader.read_u32(binding.player)
            || !reader.read_u32(binding.delay_frames) || !reader.read_u32(types) || !reader.read_u32(objects)
            || types == 0 || types > 1024U || types > (end - reader.offset()) / 8U
            || objects > (end - reader.offset()) / 8U - types) {
            return fail<Parsed>(diagnostic_codes::malformed, "invalid GARR extension row", logical_path);
        }
        binding.templates.resize(types);
        binding.registered.resize(objects);
        for (auto& type : binding.templates) if (!reader.read_u64(type))
            return fail<Parsed>(diagnostic_codes::malformed, "truncated GARR template", logical_path);
        for (auto& id : binding.registered) if (!reader.read_u64(id))
            return fail<Parsed>(diagnostic_codes::malformed, "truncated GARR object", logical_path);
        replay.setup.free_garrisons.push_back(std::move(binding));
    }
    if (reader.offset() != end)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid GARR extension length", logical_path);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_garrison_disabled(
    Reader& reader,
    const std::string_view logical_path,
    std::vector<EntityId>& disabled_garrisons,
    const std::uint16_t length) {
    using Parsed = void;
    if (length == 0 || length % 8U != 0) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid GSPN extension length", logical_path);
    }
    EntityId previous{};
    for (std::size_t offset = 0; offset < length; offset += 8U) {
        EntityId id{};
        if (!reader.read_u64(id) || id <= previous) {
            return fail<Parsed>(diagnostic_codes::malformed,
                "GSPN entity IDs must be nonzero and strictly increasing", logical_path);
        }
        disabled_garrisons.push_back(id);
        previous = id;
    }
    return core::Result<void>::success();
}

core::Result<void> parse_extension_queue_identities(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    std::uint32_t version{};
    if (length != 4 || !reader.read_u32(version) || version != 1) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid QIDS extension body", logical_path);
    }
    replay.setup.queue_identities = true;
    return core::Result<void>::success();
}

core::Result<void> parse_extension_skirmish_setup(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    if (replay.setup.skirmish || length > 65000U) {
        return fail<Parsed>(diagnostic_codes::malformed, "duplicate or oversized SKSU extension", logical_path);
    }
    Reader body(bytes.subspan(reader.offset(), length));
    ReplaySkirmishSetup metadata;
    if (!read_skirmish(body, metadata)) {
        return fail<Parsed>(diagnostic_codes::malformed, "invalid SKSU extension body", logical_path);
    }
    std::vector<std::uint8_t> consumed(length);
    static_cast<void>(reader.read_bytes(consumed));
    replay.setup.skirmish = std::move(metadata);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_beacon_rules(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    std::uint32_t version{}, rows{};
    if (!reader.read_u32(version) || !reader.read_u32(rows) || version != 1
        || rows == 0 || rows > max_players || length != 8U + 16U * rows)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid beacon rules extension", logical_path);
    replay.setup.beacon_rules.reserve(rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        TacticalSetup::BeaconRule rule;
        if (!reader.read_u32(rule.player) || !reader.read_u32(rule.lifetime_frames) || !reader.read_u64(rule.type))
            return fail<Parsed>(diagnostic_codes::malformed, "truncated beacon rules row", logical_path);
        replay.setup.beacon_rules.push_back(rule);
    }
    return core::Result<void>::success();
}

core::Result<void> parse_extension_attract_pool(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    std::uint32_t version{}, rows{};
    if (replay.setup.attract_pool || !reader.read_u32(version) || !reader.read_u32(rows) || version != 1
        || length != 8U + 8U * static_cast<std::uint64_t>(rows) || rows > max_units)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid or duplicate ATRC extension", logical_path);
    AttractPoolReturn pool;
    pool.types.reserve(rows);
    for (std::uint32_t row = 0; row < rows; ++row) {
        TypeId type{};
        if (!reader.read_u64(type) || type == 0 || (!pool.types.empty() && type <= pool.types.back()))
            return fail<Parsed>(diagnostic_codes::malformed,
                "ATRC types must be nonzero and strictly increasing", logical_path);
        pool.types.push_back(type);
    }
    replay.setup.attract_pool = std::move(pool);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_native_scene(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t length) {
    using Parsed = void;
    if (length > reader.remaining() || length > replay_max_bytes)
        return fail<Parsed>(diagnostic_codes::malformed, "truncated native scene body", logical_path);
    std::vector<std::uint8_t> encoded(length);
    if (replay.setup.native_scene || !reader.read_bytes(encoded))
        return fail<Parsed>(diagnostic_codes::malformed, "duplicate or truncated native scene", logical_path);
    Reader body(encoded);
    ReplayNativeScene scene;
    std::uint32_t version{}, flags{};
    if (!body.read_u32(version) || version != 1 || !body.read_u32(flags) || (flags & ~1U) != 0)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid native scene version or flags", logical_path);
    scene.multiplayer = flags != 0;
    for (auto* names : {&scene.source_types, &scene.source_obstacles, &scene.battle_types}) {
        std::uint32_t name_count{};
        if (!body.read_u32(name_count) || name_count > body.remaining() / 3U)
            return fail<Parsed>(diagnostic_codes::malformed, "invalid native scene count", logical_path);
        for (std::uint32_t row = 0; row < name_count; ++row) {
            std::uint16_t size{};
            if (!body.read_u16(size) || size == 0 || size > body.remaining())
                return fail<Parsed>(diagnostic_codes::malformed, "invalid native scene type length", logical_path);
            std::string name(size, '\0');
            if (!body.read_bytes(std::span(reinterpret_cast<std::uint8_t*>(name.data()), name.size())))
                return fail<Parsed>(diagnostic_codes::malformed, "truncated native scene type", logical_path);
            names->push_back(std::move(name));
        }
    }
    if (body.remaining() != 0)
        return fail<Parsed>(diagnostic_codes::malformed, "trailing native scene bytes", logical_path);
    replay.setup.native_scene = std::move(scene);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_object_turn(
    Reader& reader,
    const std::string_view logical_path,
    std::uint32_t& object_turn_count,
    const std::uint16_t length) {
    using Parsed = void;
    std::uint32_t version{};
    if (object_turn_count != 0 || length != 8 || !reader.read_u32(version) || version != 1
        || !reader.read_u32(object_turn_count) || object_turn_count == 0 || object_turn_count > max_commands)
        return fail<Parsed>(diagnostic_codes::malformed, "invalid or duplicate OBJT extension", logical_path);
    return core::Result<void>::success();
}

core::Result<void> parse_extension_match_policy(
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint16_t length) {
    using Parsed = void;
    if (replay.setup.match_policy || length != 4) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "duplicate or invalid match-policy header extension", logical_path);
    }
    std::uint32_t flags{};
    if (!reader.read_u32(flags) || flags == 0 || (flags & ~15U) != 0) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "match-policy extension requires nonzero supported disabled flags", logical_path);
    }
    replay.setup.match_policy = SkirmishMatchPolicy{
        (flags & 1U) == 0, (flags & 2U) == 0, (flags & 4U) == 0, (flags & 8U) == 0};
    return core::Result<void>::success();
}

core::Result<void> parse_header_extensions(
    const std::span<const std::uint8_t> bytes,
    Reader& reader,
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_size,
    const bool extensions,
    std::vector<EntityId>& disabled_garrisons,
    std::vector<UnitState>& carriers,
    std::uint32_t& object_turn_count) {
    using Parsed = void;
    if (extensions) {
        std::uint32_t count{};
        if (!reader.read_u32(count) || count == 0 || count > (header_size - reader.offset()) / 4U) {
            return fail<Parsed>(diagnostic_codes::malformed, "invalid replay header extension count", logical_path);
        }
        std::uint16_t previous_tag{};
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint16_t tag{}, length{};
            std::uint32_t native_length{};
            if (reader.offset() > header_size || header_size - reader.offset() < 4U
                || !reader.read_u16(tag)) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated replay header extension", logical_path);
            }
            const bool native = tag == replay_extension_native_scene;
            const auto format = peek_replay_format_version(bytes);
            if (native && format != replay_format_version_native && format != replay_format_version_squadron_native)
                return fail<Parsed>(diagnostic_codes::version, "native scene requires wide replay framing", logical_path);
            if ((native ? (header_size - reader.offset() < 4U || !reader.read_u32(native_length)) : !reader.read_u16(length))
                || (native ? native_length : length) > header_size - reader.offset()) {
                return fail<Parsed>(diagnostic_codes::malformed, "truncated replay header extension", logical_path);
            }
            if (tag <= previous_tag) {
                return fail<Parsed>(diagnostic_codes::malformed, "replay header extension tags must increase", logical_path);
            }
            previous_tag = tag;
            if (tag == replay_extension_controllers) {
                Reader body(bytes.subspan(reader.offset(), length));
                const auto end = reader.offset() + length;
                std::uint32_t version{}, controller_count{};
                BattleControllerPolicy policy;
                if (!body.read_u32(version) || version != 4)
                    return fail<Parsed>(diagnostic_codes::version, "unsupported controller policy version", logical_path);
                if (!body.read_u32(controller_count) || controller_count > max_controller_seats || controller_count > body.remaining() / 16U)
                    return fail<Parsed>(diagnostic_codes::malformed, "invalid controller roster length", logical_path);
                policy.seats.resize(controller_count);
                for (auto& seat : policy.seats) {
                    std::uint32_t kind{}, active{};
                    if (!body.read_u32(seat.seat.value) || !body.read_u32(seat.player)
                        || !body.read_u32(kind) || kind > 255 || !body.read_u32(active) || active > 1)
                        return fail<Parsed>(diagnostic_codes::malformed, "invalid controller seat", logical_path);
                    seat.kind = static_cast<SeatKind>(kind); seat.active = active != 0;
                }
                if (!body.read_u32(controller_count) || controller_count > max_players || controller_count > body.remaining() / 8U)
                    return fail<Parsed>(diagnostic_codes::malformed, "invalid controller faction length", logical_path);
                policy.factions.resize(controller_count);
                for (auto& faction : policy.factions) {
                    std::uint32_t mode{};
                    if (!body.read_u32(faction.player) || !body.read_u32(mode))
                        return fail<Parsed>(diagnostic_codes::malformed, "truncated controller faction", logical_path);
                    faction.mode = static_cast<FactionController>(mode);
                }
                if (!body.read_u32(controller_count) || controller_count > max_units || controller_count > body.remaining() / 16U)
                    return fail<Parsed>(diagnostic_codes::malformed, "invalid controller unit table length", logical_path);
                replay.setup.units.resize(controller_count);
                for (auto& unit : replay.setup.units) {
                    std::uint32_t control{};
                    if (!body.read_u64(unit.entity_id) || !body.read_u32(unit.controller_seat.value) || !body.read_u32(control))
                        return fail<Parsed>(diagnostic_codes::malformed, "truncated controller unit", logical_path);
                    unit.control = static_cast<UnitControl>(control);
                }
                if (body.remaining() != 0)
                    return fail<Parsed>(diagnostic_codes::malformed, "controller extension length mismatch", logical_path);
                std::vector<std::uint8_t> consumed(length);
                if (!reader.read_bytes(consumed) || reader.offset() != end)
                    return fail<Parsed>(diagnostic_codes::malformed, "truncated controller extension", logical_path);
                replay.setup.controllers = std::move(policy);
                continue;
            }
            if (tag == replay_extension_battle_domain) {
                std::uint32_t version{}, domain{};
                if (length != 8 || !reader.read_u32(version) || !reader.read_u32(domain)) {
                    return fail<Parsed>(diagnostic_codes::malformed, "invalid battle domain extension length", logical_path);
                }
                if (version != 1) {
                    return fail<Parsed>(diagnostic_codes::version, "unsupported battle domain extension version", logical_path);
                }
                if (domain != static_cast<std::uint32_t>(BattleDomain::land)) {
                    return fail<Parsed>(diagnostic_codes::malformed, "battle domain extension requires land; space must be omitted", logical_path);
                }
                replay.setup.domain = BattleDomain::land;
                continue;
            }
            if (tag == replay_extension_cinematic_seats) {
                if (auto section = parse_extension_cinematic_seats(bytes, reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_match_identity) {
                if (auto section = parse_extension_match_identity(bytes, reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_carriers) {
                if (auto section = parse_extension_carriers(bytes, reader, logical_path, carriers, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_campaign) {
                if (auto section = parse_extension_campaign(bytes, reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_first_free_id) {
                if (auto section = parse_extension_first_free_id(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_multiplayer_departure) {
                if (auto section = parse_extension_multiplayer_departure(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_free_garrison) {
                if (auto section = parse_extension_free_garrison(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_garrison_disabled) {
                if (auto section = parse_extension_garrison_disabled(reader, logical_path, disabled_garrisons, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_queue_identities) {
                if (auto section = parse_extension_queue_identities(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_skirmish_setup) {
                if (auto section = parse_extension_skirmish_setup(bytes, reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_beacon_rules) {
                if (auto section = parse_extension_beacon_rules(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_attract_pool) {
                if (auto section = parse_extension_attract_pool(reader, logical_path, replay, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_native_scene) {
                if (auto section = parse_extension_native_scene(reader, logical_path, replay, native_length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag == replay_extension_object_turn) {
                if (auto section = parse_extension_object_turn(reader, logical_path, object_turn_count, length); !section) {
                    return core::Result<void>::failure(section.error());
                }
                continue;
            }
            if (tag != replay_extension_match_policy) {
                return fail<Parsed>(diagnostic_codes::version,
                    "unknown replay header extension tag " + std::to_string(tag), logical_path);
            }
            if (auto section = parse_extension_match_policy(reader, logical_path, replay, length); !section) {
                return core::Result<void>::failure(section.error());
            }
        }
        if (reader.offset() != header_size
            || (!replay.setup.match_policy && !replay.setup.skirmish && !replay.setup.queue_identities
                && disabled_garrisons.empty() && replay.setup.free_garrisons.empty()
                && !replay.setup.first_free_entity_id && !replay.setup.campaign && carriers.empty()
                && replay.setup.cinematic_seats.empty() && !replay.setup.match_identity
                && !replay.setup.departure_policy && replay.setup.beacon_rules.empty()
                && !replay.setup.attract_pool && !replay.setup.native_scene && object_turn_count == 0
                && replay.setup.domain == BattleDomain::space)) {
            return fail<Parsed>(diagnostic_codes::malformed, "invalid replay header extension length", logical_path);
        }
    }
    return core::Result<void>::success();
}

core::Result<void> validate_header_counts(
    const std::string_view logical_path,
    TacticalReplay& replay,
    const std::uint32_t header_reserved,
    const std::uint32_t player_count,
    const std::uint64_t unit_count,
    const std::uint64_t command_count,
    const bool squadrons) {
    using Parsed = void;
    // Version 3 stores its squadron count in the version-2 reserved field; a version-3 file
    // without squadrons would be a second encoding of a version-2 replay.
    if (!squadrons && header_reserved != 0) {
        return fail<Parsed>(diagnostic_codes::version,
            "nonzero reserved replay-v2 header field", logical_path);
    }
    if (squadrons && (header_reserved == 0 || header_reserved > max_units)) {
        return fail<Parsed>(diagnostic_codes::malformed,
            "replay-v3 squadron count must be 1 to 1,000,000, found " + std::to_string(header_reserved), logical_path);
    }
    if (player_count > max_players || unit_count > max_units || command_count > max_commands
        || replay.final_tick_count > max_ticks) {
        return fail<Parsed>(diagnostic_codes::resource_limit,
            "tick/player/unit/command count exceeds tactical replay resource limits", logical_path);
    }
    return core::Result<void>::success();
}

} // namespace eawr::sim::tactical::replay_parse_detail
