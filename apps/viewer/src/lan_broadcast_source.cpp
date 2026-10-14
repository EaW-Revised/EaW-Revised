#include "lan_broadcast_source.hpp"

#include "eawr/core/build_identity.hpp"
#include "eawr/net/tcp_transport.hpp"

#include <utility>

namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;
namespace {

[[nodiscard]] model::LobbyPlace place_of(const net::LanLocation location) {
    switch (location) {
    case net::LanLocation::hosting: return model::LobbyPlace::hosting;
    case net::LanLocation::joining: return model::LobbyPlace::joining;
    case net::LanLocation::lobby: break;
    }
    return model::LobbyPlace::lobby;
}

[[nodiscard]] net::LanLocation location_of(const model::LobbyPlace place) {
    switch (place) {
    case model::LobbyPlace::hosting: return net::LanLocation::hosting;
    case model::LobbyPlace::joining: return net::LanLocation::joining;
    case model::LobbyPlace::lobby: break;
    }
    return net::LanLocation::lobby;
}

[[nodiscard]] model::LobbyPlayer player_of(const net::LanPlayer& player) {
    model::LobbyPlayer value;
    value.id = player.id;
    value.name = player.name;
    value.colour = player.colour;
    value.place = place_of(player.location);
    value.game = player.game;
    return value;
}

} // namespace

std::shared_ptr<BroadcastLobbySource> BroadcastLobbySource::open(Options options, std::string& failure) {
    net::LanBroadcastOptions broadcast;
    broadcast.listen_port = options.port;
    broadcast.extra_ports = std::move(options.extra_ports);
    // The version a game is listed with: the transport's protocol number and this build.
    broadcast.protocol = net::TcpOptions{}.protocol_version;
    broadcast.build = std::string(core::build_identity());
    auto opened = net::LanBroadcaster::open(std::move(broadcast));
    if (!opened) {
        failure = opened.error().message;
        return nullptr;
    }
    return std::shared_ptr<BroadcastLobbySource>(new BroadcastLobbySource(std::move(opened).value()));
}

void BroadcastLobbySource::announce(const model::LobbyPresence& presence) {
    net::LanPresence announced;
    announced.name = presence.name;
    announced.colour = presence.colour;
    announced.location = location_of(presence.place);
    announced.game = presence.game;
    announced.map = presence.map;
    announced.game_type = presence.game_type;
    announced.tcp_port = presence.tcp_port;
    announced.max_players = presence.max_players;
    announced.passworded = presence.passworded;
    announced.locked = presence.locked;
    broadcaster_->set_presence(std::move(announced));
}

void BroadcastLobbySource::send_chat(const model::LobbyChatScope scope, const std::string& game,
                                     const std::string& name, const std::string& text) {
    broadcaster_->send_chat(scope == model::LobbyChatScope::game ? net::ChatScope::game : net::ChatScope::lobby, game,
                            name, text);
}

bool BroadcastLobbySource::pump() { return broadcaster_->pump(net::LanClock::now()); }

std::vector<model::LobbyGame> BroadcastLobbySource::games() const {
    std::vector<model::LobbyGame> listed;
    for (const net::LanHostedGame& hosted : broadcaster_->directory().games()) {
        if (hosted.locked) continue;
        model::LobbyGame game;
        game.name = hosted.name;
        game.map = hosted.map;
        game.game_type = hosted.game_type;
        game.address = hosted.address;
        game.players = hosted.players;
        game.max_players = hosted.max_players;
        game.passworded = hosted.passworded;
        game.version_matches = hosted.version_matches;
        game.player_names = hosted.player_names;
        game.host = hosted.host;
        listed.push_back(std::move(game));
    }
    return listed;
}

std::vector<model::LobbyPlayer> BroadcastLobbySource::players() const {
    std::vector<model::LobbyPlayer> listed;
    for (const net::LanPlayer& player : broadcaster_->directory().players()) listed.push_back(player_of(player));
    return listed;
}

std::vector<model::LobbyPlayer> BroadcastLobbySource::joining(const std::string& game) const {
    std::vector<model::LobbyPlayer> listed;
    for (const net::LanPlayer& player : broadcaster_->directory().joining(game)) listed.push_back(player_of(player));
    return listed;
}

std::vector<model::LobbyChatMessage> BroadcastLobbySource::take_chat() {
    std::vector<model::LobbyChatMessage> received;
    for (const net::LanChat& chat : broadcaster_->directory().take_chat()) {
        model::LobbyChatMessage message;
        message.scope = chat.scope == net::ChatScope::game ? model::LobbyChatScope::game : model::LobbyChatScope::lobby;
        message.game = chat.game;
        message.name = chat.name;
        message.text = chat.text;
        received.push_back(std::move(message));
    }
    return received;
}

} // namespace eawr::presentation::godot_backend
