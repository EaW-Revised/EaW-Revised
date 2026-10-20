#include "live_save_transport.hpp"
#include <algorithm>
#include <bit>
#include <limits>

namespace eawr::platform::save_detail {
namespace {
namespace gc = sim::galactic;
template<class T> core::Result<T> fail(std::string message) {
    core::Diagnostic d; d.code = std::string(checkpoint::diagnostic_codes::section_invalid);
    d.message = "live input: " + message; return core::Result<T>::failure(std::move(d));
}
void length(std::vector<std::uint8_t>& out, std::uint32_t n) {
    for (unsigned i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(n >> (8U * i)));
}
}
core::Result<checkpoint::Section> encode_input(const gc::GalacticSetup& setup,
    std::uint64_t frontier, const Input& input) {
    checkpoint::Section result{input_section, 3, false, {}};
    gc::GalacticReplay orders{setup, frontier, {}}, commands{setup, frontier, input.commands};
    for (std::size_t i = 0; i < input.orders.size(); ++i) {
        const auto& order = input.orders[i];
        orders.commands.push_back({{order.tick.value_or(frontier), order.seat, i + 1}, order.payload});
    }
    for (auto* replay : {&orders, &commands}) {
        for (const auto& command : replay->commands) {
            if (command.key.tick < frontier || command.key.tick >= gc::galactic_max_ticks)
                return fail<checkpoint::Section>("queued command is outside the saved frontier");
            replay->final_tick_count = std::max(replay->final_tick_count, command.key.tick + 1);
        }
        std::vector<std::size_t> indices;
        for (std::size_t i = 0; i < replay->commands.size(); ++i) indices.push_back(i);
        std::stable_sort(indices.begin(), indices.end(), [&](auto a, auto b) { return replay->commands[a].key < replay->commands[b].key; });
        std::vector<gc::GameCommand> sorted; sorted.reserve(indices.size());
        for (const auto i : indices) sorted.push_back(std::move(replay->commands[i]));
        replay->commands = std::move(sorted);
        auto bytes = gc::write_replay(*replay);
        if (!bytes) return core::Result<checkpoint::Section>::failure(bytes.error());
        if (result.bytes.size() > checkpoint::max_bytes - 4
            || bytes.value().size() > checkpoint::max_bytes - result.bytes.size() - 4)
            return fail<checkpoint::Section>("queued input exceeds the byte budget");
        length(result.bytes, static_cast<std::uint32_t>(bytes.value().size()));
        result.bytes.insert(result.bytes.end(), bytes.value().begin(), bytes.value().end());
    }
    return core::Result<checkpoint::Section>::success(std::move(result));
}
core::Result<Input> decode_input(const checkpoint::Checkpoint& saved) {
    const auto at = std::find_if(saved.sections.begin(), saved.sections.end(), [](const auto& s) { return s.id == input_section; });
    if (at == saved.sections.end()) return core::Result<Input>::success({});
    if (at->version != 3) return fail<Input>("unsupported section version");
    std::size_t cursor{}; Input input;
    for (unsigned i = 0; i < 2; ++i) {
        if (at->bytes.size() - cursor < 4) return fail<Input>("missing replay length");
        std::uint32_t size{};
        for (unsigned b = 0; b < 4; ++b) size |= static_cast<std::uint32_t>(at->bytes[cursor++]) << (8U * b);
        if (size > at->bytes.size() - cursor) return fail<Input>("truncated replay");
        auto replay = gc::parse_replay(std::span(at->bytes).subspan(cursor, size)); cursor += size;
        if (!replay) return core::Result<Input>::failure(replay.error());
        if (replay.value().setup.content_identity != saved.header.identities.content
            || replay.value().setup.campaign_identity != saved.header.identities.campaign
            || replay.value().setup.seed != saved.header.seed)
            return fail<Input>("queued input identity differs");
        auto& commands = replay.value().commands;
        std::vector<std::size_t> indices;
        for (std::size_t n = 0; n < commands.size(); ++n) indices.push_back(n);
        if (i == 0) std::sort(indices.begin(), indices.end(), [&](auto a, auto b) {
            return commands[a].key.sequence < commands[b].key.sequence;
        });
        for (std::size_t n = 0; n < indices.size(); ++n) {
            auto& command = commands[indices[n]];
            if (command.key.tick < saved.header.completed_tick) return fail<Input>("queued command precedes frontier");
            if (i == 0) {
                if (command.key.sequence != n + 1) return fail<Input>("deferred order indices are not contiguous");
                input.orders.push_back({command.key.seat, std::move(command.payload), command.key.tick});
            }
            else input.commands.push_back(std::move(command));
        }
    }
    if (cursor != at->bytes.size()) return fail<Input>("trailing input bytes");
    return core::Result<Input>::success(std::move(input));
}
std::optional<LiveSaveCampaign> save_campaign(const LiveSaveOptions& options,
    const gc::GalacticSetup& setup, const gc::SeatId seat) {
    if (!options.campaign) return std::nullopt;
    const auto& source = *options.campaign;
    const auto row = std::find_if(setup.seats.begin(), setup.seats.end(), [&](const auto& s) { return s.seat_id == seat; });
    if (row == setup.seats.end()) return std::nullopt;
    const auto faction = std::find_if(source.factions.begin(), source.factions.end(),
        [&](const auto& f) { return f.player == row->player; });
    if (faction == source.factions.end() || faction->name.empty()) return std::nullopt;
    return LiveSaveCampaign{source.campaign, source.campaign_set, faction->name, source.difficulty,
        seat.value, source.multiplayer, source.custom};
}
checkpoint::Section encode_campaign(const LiveSaveCampaign& campaign) {
    checkpoint::Section result{static_cast<std::uint16_t>(checkpoint::SectionId::campaign_identity), 2, false, {}};
    for (const auto* text : {&campaign.campaign, &campaign.campaign_set, &campaign.faction, &campaign.difficulty}) {
        length(result.bytes, static_cast<std::uint32_t>(text->size()));
        result.bytes.insert(result.bytes.end(), text->begin(), text->end());
    }
    length(result.bytes, campaign.seat);
    result.bytes.push_back(campaign.multiplayer ? 1 : 0);
    result.bytes.push_back(campaign.custom ? 1 : 0);
    if (campaign.custom) {
        for (const auto value : {campaign.custom->credits, campaign.custom->starting_tech, campaign.custom->maximum_tech}) {
            const auto bits = std::bit_cast<std::uint64_t>(value);
            for (unsigned b = 0; b < 8; ++b) result.bytes.push_back(static_cast<std::uint8_t>(bits >> (b * 8U)));
        }
        result.bytes.push_back(campaign.custom->allow_heroes ? 1 : 0);
    }
    return result;
}
core::Result<std::optional<LiveSaveCampaign>> decode_campaign(const checkpoint::Checkpoint& saved) {
    using Result = core::Result<std::optional<LiveSaveCampaign>>;
    const auto* at = saved.find(checkpoint::SectionId::campaign_identity);
    if (!at) return Result::success(std::nullopt);
    const auto invalid = [](std::string message) {
        core::Diagnostic d; d.code = std::string(checkpoint::diagnostic_codes::section_invalid);
        d.message = "campaign identity: " + std::move(message); return Result::failure(std::move(d));
    };
    if (at->version != 2) return invalid("unsupported section version");
    std::size_t cursor{};
    const auto u32 = [&](std::uint32_t& value) {
        if (at->bytes.size() - cursor < 4) return false;
        value = 0;
        for (unsigned b = 0; b < 4; ++b) value |= static_cast<std::uint32_t>(at->bytes[cursor++]) << (8U * b);
        return true;
    };
    LiveSaveCampaign campaign;
    for (auto* text : {&campaign.campaign, &campaign.campaign_set, &campaign.faction, &campaign.difficulty}) {
        std::uint32_t size{};
        if (!u32(size) || size > at->bytes.size() - cursor) return invalid("truncated name");
        text->assign(at->bytes.begin() + static_cast<std::ptrdiff_t>(cursor),
            at->bytes.begin() + static_cast<std::ptrdiff_t>(cursor + size));
        cursor += size;
    }
    if (!u32(campaign.seat) || cursor == at->bytes.size() || at->bytes[cursor] > 1) return invalid("truncated seat");
    campaign.multiplayer = at->bytes[cursor++] != 0;
    if (cursor == at->bytes.size() || at->bytes[cursor] > 1) return invalid("truncated custom options");
    if (at->bytes[cursor++] != 0) {
        if (at->bytes.size() - cursor < 25) return invalid("truncated custom economy");
        campaign.custom.emplace();
        for (auto* value : {&campaign.custom->credits, &campaign.custom->starting_tech, &campaign.custom->maximum_tech}) {
            std::uint64_t bits{};
            for (unsigned b = 0; b < 8; ++b) bits |= std::uint64_t{at->bytes[cursor++]} << (b * 8U);
            *value = std::bit_cast<std::int64_t>(bits);
        }
        if (at->bytes[cursor] > 1) return invalid("invalid custom hero flag");
        campaign.custom->allow_heroes = at->bytes[cursor++] != 0;
    }
    if (cursor != at->bytes.size()) return invalid("trailing bytes");
    if (campaign.campaign.empty() || campaign.faction.empty() || campaign.seat == 0) return invalid("empty campaign, faction or seat");
    return Result::success(std::move(campaign));
}
} // namespace eawr::platform::save_detail
