#include "eawr/net/galactic_setup.hpp"
#include "eawr/net/lan_directory.hpp"

#include <algorithm>
#include <set>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
namespace gc = sim::galactic;
constexpr std::string_view magic = "GCSU";

template<class T> core::Result<T> refuse(SetupReason reason, std::string message) {
    return core::Result<T>::failure(SetupRefusal{reason, {}, std::move(message), {}, {}}.diagnostic());
}
void word(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}
void blob(Bytes& bytes, std::span<const std::uint8_t> value) {
    word(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void text(Bytes& bytes, std::string_view value) {
    blob(bytes, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool word(std::uint64_t& value, unsigned width) {
        if (bytes.size() < width) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
        bytes = bytes.subspan(width);
        return true;
    }
    bool flag(bool& value) {
        std::uint64_t raw{};
        if (!word(raw, 1) || raw > 1) return false;
        value = raw == 1;
        return true;
    }
    bool blob(Bytes& value, std::size_t limit) {
        std::uint64_t size{};
        if (!word(size, 4) || size > limit || size > bytes.size()) return false;
        value.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size));
        return true;
    }
    // Hashed text stays byte-exact: anything the display sanitiser would repair is refused.
    bool text(std::string& value, std::size_t limit) {
        Bytes data;
        if (!blob(data, limit)) return false;
        const std::string_view raw(reinterpret_cast<const char*>(data.data()), data.size());
        if (sanitize_lan_text(raw, limit) != raw) return false;
        value = std::string(raw);
        return true;
    }
};
}

core::Result<Bytes> encode_galactic_setup(const GalacticSetupPayload& payload) {
    const auto& roster = payload.roster;
    if (auto valid = gc::validate_mp_roster(roster); !valid) return refuse<Bytes>(SetupReason::options, valid.error().message);
    if (payload.command_quota != max_batch_commands || payload.byte_quota != max_batch_bytes)
        return refuse<Bytes>(SetupReason::options, "batch quotas differ from the session limits");
    Bytes bytes(magic.begin(), magic.end());
    word(bytes, 1, 4);
    text(bytes, roster.campaign); text(bytes, roster.difficulty);
    word(bytes, roster.custom ? 1U : 0U, 1);
    word(bytes, static_cast<std::uint64_t>(roster.credits), 8);
    word(bytes, static_cast<std::uint64_t>(roster.starting_tech), 8);
    word(bytes, static_cast<std::uint64_t>(roster.maximum_tech), 8);
    word(bytes, roster.allow_heroes ? 1U : 0U, 1);
    word(bytes, payload.command_quota, 4); word(bytes, payload.byte_quota, 4);
    word(bytes, roster.players.size(), 4);
    for (const auto& player : roster.players) {
        word(bytes, player.player, 4); word(bytes, player.faction_id, 4); word(bytes, player.flags, 4);
        text(bytes, player.faction);
        word(bytes, player.active ? 1U : 0U, 1); word(bytes, static_cast<std::uint8_t>(player.dropout), 1);
        blob(bytes, player.ai_configuration);
    }
    word(bytes, roster.seats.size(), 4);
    for (const auto& seat : roster.seats) {
        word(bytes, seat.seat_id.value, 4); word(bytes, seat.player, 4);
        word(bytes, static_cast<std::uint8_t>(seat.kind), 1); word(bytes, seat.lobby_slot, 4);
        word(bytes, seat.active ? 1U : 0U, 1); word(bytes, seat.human_side_id, 4);
    }
    word(bytes, roster.teams.size(), 4);
    for (const auto& team : roster.teams) {
        word(bytes, team.side, 4); word(bytes, team.lead.value, 4); word(bytes, team.observer_ai ? 1U : 0U, 1);
    }
    bytes.insert(bytes.end(), roster.tick_zero.begin(), roster.tick_zero.end());
    if (bytes.size() > max_setup_bytes - 16384)
        return refuse<Bytes>(SetupReason::options, "campaign setup exceeds offer budget");
    return core::Result<Bytes>::success(std::move(bytes));
}

core::Result<GalacticSetupPayload> decode_galactic_setup(std::span<const std::uint8_t> input) {
    using Result = core::Result<GalacticSetupPayload>;
    const auto malformed = [] { return refuse<GalacticSetupPayload>(SetupReason::malformed, "malformed campaign setup payload"); };
    if (input.size() < magic.size() + 4 || input.size() > max_setup_bytes
        || !std::equal(magic.begin(), magic.end(), input.begin())) return malformed();
    Reader reader{input.subspan(magic.size())};
    GalacticSetupPayload payload;
    auto& roster = payload.roster;
    std::uint64_t version{}, credits{}, start{}, maximum{}, quota_commands{}, quota_bytes{}, count{};
    if (!reader.word(version, 4) || version != 1 || !reader.text(roster.campaign, gc::mp_roster_max_text)
        || !reader.text(roster.difficulty, gc::mp_roster_max_text) || !reader.flag(roster.custom)
        || !reader.word(credits, 8) || !reader.word(start, 8) || !reader.word(maximum, 8)
        || !reader.flag(roster.allow_heroes) || !reader.word(quota_commands, 4) || !reader.word(quota_bytes, 4)
        || !reader.word(count, 4) || count > gc::galactic_max_players) return malformed();
    roster.credits = static_cast<std::int64_t>(credits);
    roster.starting_tech = static_cast<std::int64_t>(start);
    roster.maximum_tech = static_cast<std::int64_t>(maximum);
    payload.command_quota = static_cast<std::uint32_t>(quota_commands);
    payload.byte_quota = static_cast<std::uint32_t>(quota_bytes);
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::MpRosterPlayer player;
        std::uint64_t id{}, faction_id{}, flags{}, dropout{};
        if (!reader.word(id, 4) || !reader.word(faction_id, 4) || !reader.word(flags, 4)
            || !reader.text(player.faction, gc::mp_roster_max_text) || !reader.flag(player.active)
            || !reader.word(dropout, 1) || !reader.blob(player.ai_configuration, gc::mp_roster_max_ai_configuration))
            return malformed();
        player.player = static_cast<gc::PlayerId>(id); player.faction_id = static_cast<gc::FactionId>(faction_id);
        player.flags = static_cast<std::uint32_t>(flags);
        player.dropout = static_cast<gc::FactionControl>(dropout);
        roster.players.push_back(std::move(player));
    }
    if (!reader.word(count, 4) || count > gc::galactic_max_seats) return malformed();
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::Seat seat;
        std::uint64_t id{}, player{}, kind{}, slot{}, side{};
        if (!reader.word(id, 4) || !reader.word(player, 4) || !reader.word(kind, 1) || !reader.word(slot, 4)
            || !reader.flag(seat.active) || !reader.word(side, 4)) return malformed();
        seat.seat_id = sim::SeatId{static_cast<std::uint32_t>(id)}; seat.player = static_cast<gc::PlayerId>(player);
        seat.kind = static_cast<sim::SeatKind>(kind); seat.lobby_slot = static_cast<std::uint32_t>(slot);
        seat.human_side_id = static_cast<gc::HumanSideId>(side);
        roster.seats.push_back(seat);
    }
    if (!reader.word(count, 4) || count > 2) return malformed();
    for (std::uint64_t i = 0; i < count; ++i) {
        gc::TeamLead team;
        std::uint64_t side{}, lead{};
        if (!reader.word(side, 4) || !reader.word(lead, 4) || !reader.flag(team.observer_ai)) return malformed();
        team.side = static_cast<gc::HumanSideId>(side); team.lead = sim::SeatId{static_cast<std::uint32_t>(lead)};
        roster.teams.push_back(team);
    }
    if (reader.bytes.size() != roster.tick_zero.size()) return malformed();
    std::copy(reader.bytes.begin(), reader.bytes.end(), roster.tick_zero.begin());
    // Encoding validates roster enum values; comparison then enforces canonical bytes.
    auto encoded = encode_galactic_setup(payload);
    if (!encoded) return Result::failure(encoded.error());
    if (!std::equal(input.begin(), input.end(), encoded.value().begin(), encoded.value().end())) return malformed();
    return Result::success(std::move(payload));
}

core::Result<void> validate_galactic_offer(const SetupOffer& offer) {
    if (offer.seats.size() > gc::galactic_max_human_seats)
        return refuse<void>(SetupReason::options, "a campaign setup has at most eight human seats");
    auto payload = decode_galactic_setup(offer.setup);
    if (!payload) return core::Result<void>::failure(payload.error());
    const auto& roster = payload.value().roster;
    // Peers and seats: exactly the human seats of the roster, each once, on the faction the roster names.
    std::set<sim::SeatId> offered;
    for (std::size_t index = 0; index < offer.seats.size(); ++index) {
        const auto& seat = offer.seats[index];
        if (seat.peer != index) return refuse<void>(SetupReason::options, "peer indices are not canonical");
        const auto found = std::find_if(roster.seats.begin(), roster.seats.end(), [&](const auto& row) {
            return row.seat_id == seat.seat; });
        if (found == roster.seats.end() || found->kind != sim::SeatKind::human || !found->active
            || found->player != seat.player || !offered.insert(seat.seat).second)
            return refuse<void>(SetupReason::options, "peer seat is not an active human seat of the campaign roster");
    }
    const auto humans = static_cast<std::size_t>(std::count_if(roster.seats.begin(), roster.seats.end(),
        [](const auto& seat) { return seat.kind == sim::SeatKind::human; }));
    if (humans != offer.seats.size())
        return refuse<void>(SetupReason::options, "every human seat of the campaign roster needs one peer");
    const auto zero = std::find_if(offer.identity.content.domains.begin(), offer.identity.content.domains.end(),
        [](const auto& domain) { return domain.name == galactic_tick_zero_domain; });
    if (zero == offer.identity.content.domains.end() || zero->digest != roster.tick_zero)
        return refuse<void>(SetupReason::options, "offer identity and campaign setup disagree on the tick-zero digest");
    return core::Result<void>::success();
}

} // namespace eawr::net
