#include "lan_match_internal.hpp"

namespace eawr::presentation::godot_backend::lan_detail {

std::string_view command_kind(const tactical::CommandPayload& payload) {
    if (std::holds_alternative<tactical::MovePayload>(payload)) return "move";
    if (std::holds_alternative<tactical::AttackPayload>(payload)
        || std::holds_alternative<tactical::AttackMovePayload>(payload)) return "attack";
    if (std::holds_alternative<tactical::AbilityPayload>(payload)
        || std::holds_alternative<tactical::AreaAbilityPayload>(payload)) return "ability";
    if (std::holds_alternative<tactical::QuitPayload>(payload)) return "quit";
    return "other";
}

InputTime input_time(std::uint64_t tick, std::uint64_t frame) {
    using namespace std::chrono;
    return {tick, frame, duration_cast<microseconds>(system_clock::now().time_since_epoch()).count(),
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count()};
}

core::Diagnostic diagnostic(std::string code, std::string message) {
    core::Diagnostic result; result.code = std::move(code); result.message = std::move(message); return result;
}

core::Result<skirmish::FixtureOptions> fixture_options(const net::SetupOffer& offer, std::span<const std::string> factions) {
    auto decoded = net::decode_skirmish_setup(offer.setup);
    if (!decoded) return core::Result<skirmish::FixtureOptions>::failure(decoded.error());
    const auto& setup = decoded.value();
    skirmish::FixtureOptions result;
    result.map = setup.skirmish->map;
    result.seed = offer.seed;
    result.match = setup.skirmish->match;
    result.victory_condition = static_cast<tactical::VictoryCondition>(setup.skirmish->victory_condition);
    result.slots.emplace();
    for (const auto& slot : setup.skirmish->slots) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const auto& value) { return value.player_id == slot.player; });
        if (player == setup.players.end()) return fail<skirmish::FixtureOptions>("Offered lobby player is missing.");
        const auto faction = std::find_if(factions.begin(), factions.end(), [&](const auto& name) {
            return skirmish::faction_id(name) == player->faction_id;
        });
        if (faction == factions.end()) return fail<skirmish::FixtureOptions>("Offered faction is unavailable in the loaded data.");
        skirmish::LobbySlot lobby{slot.player, *faction, player->team_id, slot.human, slot.fleet};
        lobby.colour_index = slot.colour_index;
        result.slots->push_back(std::move(lobby));
    }
    return core::Result<skirmish::FixtureOptions>::success(std::move(result));
}

DrainWorkers& drain_workers() { static DrainWorkers workers; return workers; }

void finish_io(std::unique_ptr<TerminalIo> io) {
    if (io->transport.tcp) {
        // Only teardown has a bounded drain. An ended match still owns its I/O
        // and must await the peer's final exchange without an elapsed-time cut-off.
        // Online GNS callbacks share a process I/O owner; teardown stays on it.
        if (io->transport.tcp->online()) { io->transport.tcp->close(); (void)io->transport.tcp->poll(); return; }
        // The detached drain must not retain the live owner's logger or state.
        if (io->transport.diagnostics) {
            for (net::PeerIndex peer = 0; peer < io->transport.peer_count(); ++peer)
                if (peer != io->transport.self()) io->transport.diagnostics->emit("link-close-request", "terminal-drain", peer);
            io->transport.diagnostics.reset();
        }
        io->deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        drain_workers().start(std::move(io));
    }
}

} // namespace eawr::presentation::godot_backend::lan_detail
