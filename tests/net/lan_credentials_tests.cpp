#include "setup_test_support.hpp"
#include "eawr/net/lan_credentials.hpp"
#include <array>
#include <deque>
#include <iostream>
#include <cstdlib>
#include <new>

namespace deletion_probe {
void* watched{};
std::size_t bytes{};
bool seen{}, erased{};
void release(void* storage) noexcept {
    if (storage && storage == watched) {
        seen = true; erased = true;
        const auto* data = static_cast<const std::uint8_t*>(storage);
        for (std::size_t index = 0; index < bytes; ++index) erased = erased && data[index] == 0;
        watched = nullptr;
    }
    std::free(storage);
}
}
// Observe the actual owned allocation immediately before release, while its
// bytes remain allocated. No reading freed vector storage and no production hook.
void* operator new(std::size_t size) {
    if (auto* storage = std::malloc(size ? size : 1)) return storage;
    throw std::bad_alloc{};
}
void operator delete(void* storage) noexcept { deletion_probe::release(storage); }
void operator delete(void* storage, std::size_t) noexcept { deletion_probe::release(storage); }

using namespace setup_test;
namespace {
class Stream final : public net::MessageTransport {
public:
    net::PeerIndex index{};
    std::uint32_t count{2};
    bool open{true}, writable{true};
    std::deque<net::Received> sent;
    eawr::core::Result<void> send(net::PeerIndex to, std::span<const std::uint8_t> bytes) override {
        if (!writable) { eawr::core::Diagnostic error; error.message = "synthetic send failure";
            return eawr::core::Result<void>::failure(std::move(error)); }
        sent.push_back({to, {bytes.begin(), bytes.end()}});
        return eawr::core::Result<void>::success();
    }
    std::optional<net::Received> poll() override { return {}; }
    net::LinkState link(net::PeerIndex) const override { return open ? net::LinkState::open : net::LinkState::closed; }
    net::PeerIndex self() const override { return index; }
    std::uint32_t peer_count() const override { return count; }
};
bool contains(std::span<const std::uint8_t> bytes, const net::RejoinNonce& token) {
    return std::search(bytes.begin(), bytes.end(), token.begin(), token.end()) != bytes.end();
}
void provision(std::uint32_t count, bool mesh_repair = false) {
    auto agreed = offer(); agreed.recovery.reconnect = !mesh_repair;
    const auto provide = [mesh_repair](auto& owner, const auto& selected, auto& stream) {
        return mesh_repair ? owner.provide_mesh_repair(selected, stream) : owner.provide(selected, stream);
    };
    agreed.setup = require(net::encode_skirmish_setup(human_setup(count)));
    agreed.seats.clear();
    for (net::PeerIndex seat = 0; seat < count; ++seat) agreed.seats.push_back({seat, sim::SeatId{seat + 1}, seat + 1});
    Stream host; host.count = count;
    net::LanReconnectCredentials provider;
    const auto tokens = require(provide(provider, agreed, host));
    expect(agreed.recovery.reconnect == !mesh_repair, "pair-key delivery never changes the offered recovery policy");
    expect(net::valid_reconnect_tokens(tokens, 0, count), "CSPRNG host pair keys are distinct");
    expect(!provider.delivered(), "no completed delivery before all acknowledgements");
    std::vector<net::ReconnectTokens> rows(count); rows[0] = tokens;
    const auto public_offer = require(net::encode_setup_offer(agreed));
    const auto replay = require(tactical::write_replay({setup(), 1, {}}));
    for (std::size_t remote = 1; remote < tokens.size(); ++remote) {
        const auto& token = tokens[remote];
        expect(std::any_of(token.begin(), token.end(), [](auto byte) { return byte != 0; }), "nonzero CSPRNG credential");
        expect(!contains(public_offer, token) && !contains(replay, token), "tokens never enter setup or replay");
    }
    for (net::PeerIndex seat = 1; seat < count; ++seat) {
        Stream join; join.index = seat; join.count = count;
        net::LanReconnectCredentials receiver;
        auto delivery = std::move(host.sent.front()); host.sent.pop_front();
        expect(delivery.from == seat, "credentials delivered only to assigned admitted stream");
        delivery.from = 0;
        expect(require(receiver.receive(delivery, join)), "delivery consumed outside simulation");
        expect(std::all_of(delivery.frame.begin(), delivery.frame.end(), [](auto byte) { return byte == 0; }), "delivery buffer erased");
        auto changed = agreed; ++changed.input_delay;
        auto wrong = provide(receiver, changed, join);
        expect(!wrong && wrong.error().code == "EAWR-NET-1023", "another match identity refused");
        rows[seat] = require(provide(receiver, agreed, join));
        expect(rows[seat].size() == count && rows[seat][0] == tokens[seat] && rows[seat][seat] == net::RejoinNonce{},
            "seat receives exactly its pair keys and no self key");
        auto ack = std::move(join.sent.front()); ack.from = seat;
        expect(require(provider.receive(ack, host)), "seat acknowledgement consumed");
        if (seat + 1 < count) expect(!provider.delivered(), "one missing acknowledgement keeps delivery incomplete");
        receiver.clear();
        expect(!provide(receiver, agreed, join), "cleared provider cannot rejoin old match");
    }
    expect(provider.delivered(), "host delivery completes only after every acknowledgement");
    for (net::PeerIndex first = 0; first < count; ++first) for (net::PeerIndex second = first + 1; second < count; ++second) {
        expect(rows[first][second] == rows[second][first], "pair endpoints share one key");
        for (net::PeerIndex stranger = 0; stranger < count; ++stranger) if (stranger != first && stranger != second)
            for (const auto& key : rows[stranger]) expect(key != rows[first][second], "another seat cannot obtain the pair key");
    }
    provider.clear();
    const auto next = require(provide(provider, agreed, host));
    for (std::size_t remote = 1; remote < next.size(); ++remote)
        for (std::size_t previous = 1; previous < tokens.size(); ++previous)
            expect(next[remote] != tokens[previous], "fresh match credentials even for identical setup");
    auto erased = next; erased.erase();
    for (const auto& token : erased) expect(std::all_of(token.begin(), token.end(), [](auto byte) { return byte == 0; }), "owned token storage zeroized");
}
// NET-1020: an online room's transport table (capacity 8) is wider than the admitted roster (3 humans on dense peers 0..2, AI rows have no peer).
void sparse_roster() {
    auto agreed = offer(); agreed.recovery.reconnect = false;
    agreed.setup = require(net::encode_skirmish_setup(human_setup(3)));
    agreed.seats.clear();
    const std::array<net::PeerIndex, 3> peers{0, 1, 2};
    for (std::size_t row = 0; row < peers.size(); ++row) agreed.seats.push_back({peers[row], sim::SeatId{static_cast<unsigned>(row) + 1}, static_cast<unsigned>(row) + 1});
    Stream host; host.count = 8;
    net::LanReconnectCredentials provider;
    const auto tokens = require(provider.provide_mesh_repair(agreed, host));
    expect(tokens.size() == 8 && net::valid_reconnect_tokens(tokens, 0, 8), "host keys cover the whole transport table");
    expect(host.sent.size() == 2 && host.sent[0].from == 1 && host.sent[1].from == 2, "credentials go only to admitted seats");
    expect(!provider.delivered(), "no delivery before the admitted seats acknowledge");
    for (const net::PeerIndex seat : {1U, 2U}) {
        Stream join; join.index = seat; join.count = 8;
        net::LanReconnectCredentials receiver;
        auto delivery = std::move(host.sent.front()); host.sent.pop_front(); delivery.from = 0;
        expect(require(receiver.receive(delivery, join)), "wide-table delivery consumed");
        const auto row = require(receiver.provide_mesh_repair(agreed, join));
        expect(row.size() == 8 && row[0] == tokens[seat], "seat receives its pair key with the host");
        auto ack = std::move(join.sent.front()); ack.from = seat;
        expect(require(provider.receive(ack, host)), "wide-table acknowledgement consumed");
    }
    expect(provider.delivered(), "delivery completes on the admitted seats alone");
    Stream outsider; outsider.index = 5; outsider.count = 8;
    net::LanReconnectCredentials stranger;
    expect(!stranger.provide_mesh_repair(agreed, outsider), "a slot outside the roster is refused");
    auto duplicate = agreed; duplicate.seats[2].peer = 1;
    net::LanReconnectCredentials repeated; host.sent.clear();
    expect(!repeated.provide_mesh_repair(duplicate, host), "duplicate roster peers refused");
    auto beyond = agreed; beyond.seats[2].peer = 8;
    expect(!repeated.provide_mesh_repair(beyond, host), "roster peer beyond the table refused");
}
void refusals() {
    auto agreed = offer(); agreed.recovery.reconnect = true;
    agreed.setup = require(net::encode_skirmish_setup(human_setup(3)));
    agreed.seats.push_back({2, sim::SeatId{12}, 3});
    Stream host, join; host.count = join.count = 3; join.index = 1;
    net::LanReconnectCredentials source;
    const auto tokens = require(source.provide(agreed, host));
    const auto original = host.sent.front().frame;
    for (const unsigned fault : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) {
        net::LanReconnectCredentials receiver;
        net::Received delivery{0, original};
        if (fault == 0) delivery.from = 1;
        if (fault == 1) delivery.frame[2] = 2;
        if (fault == 2) delivery.frame.pop_back();
        if (fault == 3) std::fill(delivery.frame.begin() + 42, delivery.frame.begin() + 74, std::uint8_t{});
        if (fault == 4) std::copy_n(delivery.frame.begin() + 42, 32, delivery.frame.begin() + 74);
        if (fault == 5) delivery.frame[1] = 2;
        if (fault == 6) delivery.frame[38] = 3;
        if (fault == 7) delivery.frame[38] = static_cast<std::uint8_t>(net::max_match_seats + 1);
        const auto result = receiver.receive(delivery, join);
        expect(!result && result.error().code == "EAWR-NET-1024", "malformed credential frame fails closed");
        for (const auto& token : tokens) {
            const auto& message = result.error().message;
            expect(!contains(std::span(reinterpret_cast<const std::uint8_t*>(message.data()), message.size()), token), "diagnostic excludes token bytes");
        }
        expect(std::all_of(delivery.frame.begin(), delivery.frame.end(), [](auto byte) { return byte == 0; }), "refused frame erased too");
    }
    net::LanReconnectCredentials receiver;
    net::Received delivered{0, original}; require(receiver.receive(delivered, join));
    net::Received duplicate{0, original};
    expect(!receiver.receive(duplicate, join), "duplicate delivery refused");
    require(receiver.provide(agreed, join));
    const auto ack = join.sent.front().frame;
    expect(ack.size() == 37, "acknowledgement has canonical size before corruption");
    for (const auto fault : {0, 1, 2}) {
        net::Received invalid{1, ack};
        if (fault == 0) invalid.from = 2;
        if (fault == 1) invalid.frame[1] = 2;
        if (fault == 2) invalid.frame[36] ^= 1;
        expect(!source.receive(invalid, host), "wrong seat or identity acknowledgement refused");
    }
    net::LanReconnectCredentials premature;
    net::Received early{1, ack};
    expect(!premature.receive(early, host), "ack before delivery refused");
    net::LanReconnectCredentials missing;
    expect(!missing.provide(agreed, join), "missing provider delivery refused");
    host.open = false; source.clear();
    expect(!source.provide(agreed, host), "non-admitted stream refused");
    host.open = true; host.writable = false;
    expect(!source.provide(agreed, host), "failed delivery cannot provide tokens");
    agreed.recovery.reconnect = false;
    expect(!source.provide(agreed, host), "disabled policy cannot obtain credentials");
}
void owned_destructor_erasure() {
    deletion_probe::seen = deletion_probe::erased = false;
    {
        net::ReconnectOptions envelope;
        envelope.seat_tokens.resize(3);
        for (auto& key : envelope.seat_tokens) key = require(net::make_rejoin_nonce());
        deletion_probe::watched = envelope.seat_tokens.data();
        deletion_probe::bytes = envelope.seat_tokens.size() * sizeof(net::RejoinNonce);
    }
    expect(deletion_probe::seen && deletion_probe::erased, "session envelope destructor erases its actual allocation before release");
}
}
int main() {
    try {
        for (const auto count : {2U, 3U, 8U, 9U}) provision(count);
        for (const auto count : {3U, 9U}) provision(count, true);
        Stream pair;
        net::LanReconnectCredentials pair_keys;
        expect(!pair_keys.provide_mesh_repair(offer(), pair), "mesh repair credentials require at least three room members");
        sparse_roster();
        refusals();
        owned_destructor_erasure();
        std::cout << "LAN reconnect credential contracts passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return 0;
}
