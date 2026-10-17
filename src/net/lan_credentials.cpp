#include "eawr/net/lan_credentials.hpp"
#include "eawr/core/sha256.hpp"
#include <algorithm>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
template<class T> core::Result<T> fail(std::string_view code, std::string message) {
    core::Diagnostic diagnostic; diagnostic.code = code; diagnostic.message = std::move(message);
    return core::Result<T>::failure(std::move(diagnostic));
}
void append(Bytes& frame, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4; ++byte) frame.push_back(static_cast<std::uint8_t>(value >> (8U * byte)));
}
std::uint32_t read(std::span<const std::uint8_t> bytes) {
    std::uint32_t value{};
    for (unsigned byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(bytes[byte]) << (8U * byte);
    return value;
}
struct EraseFrame {
    Bytes& bytes;
    ~EraseFrame() { erase_reconnect_secret(bytes); }
};
}
void LanReconnectCredentials::clear() noexcept {
    tokens_.clear(); identity_ = {}; destination_ = 0; received_ = verified_ = false; acknowledged_.clear();
}
bool LanReconnectCredentials::delivered() const noexcept {
    return verified_ && !acknowledged_.empty()
        && std::all_of(acknowledged_.begin(), acknowledged_.end(), [](bool acknowledged) { return acknowledged; });
}
core::Result<ReconnectTokens> LanReconnectCredentials::provide(const SetupOffer& offer, MessageTransport& transport) {
    if (!offer.recovery.reconnect)
        return fail<ReconnectTokens>("EAWR-NET-1020", "Reconnect credential policy or roster differs.");
    return provide_for_offer(offer, transport);
}
core::Result<ReconnectTokens> LanReconnectCredentials::provide_mesh_repair(const SetupOffer& offer, MessageTransport& transport) {
    if (transport.peer_count() < 3)
        return fail<ReconnectTokens>("EAWR-NET-1020", "Pair repair requires an online room mesh.");
    return provide_for_offer(offer, transport);
}
core::Result<ReconnectTokens> LanReconnectCredentials::provide_for_offer(const SetupOffer& offer, MessageTransport& transport) {
    if (offer.seats.size() != transport.peer_count() || transport.peer_count() < 2
        || transport.peer_count() > max_match_seats || transport.self() >= transport.peer_count())
        return fail<ReconnectTokens>("EAWR-NET-1020", "Reconnect credential policy or roster differs.");
    auto envelope = encode_setup_offer(offer);
    if (!envelope) return core::Result<ReconnectTokens>::failure(envelope.error());
    const auto identity = core::sha256(envelope.value());
    if (transport.self() == 0 && !received_) {
        identity_ = identity;
        const auto count = transport.peer_count();
        ReconnectTokens pairs(static_cast<std::size_t>(count) * count);
        for (PeerIndex first = 0; first < count; ++first) for (PeerIndex second = first + 1; second < count; ++second) {
            auto random = make_rejoin_nonce();
            if (!random) { clear(); return fail<ReconnectTokens>("EAWR-NET-1021", "OS reconnect credential generation failed."); }
            pairs[first * count + second] = pairs[second * count + first] = random.value();
            erase_reconnect_secret(random.value());
        }
        // Validate every generated unordered pair without copying key material.
        for (PeerIndex first = 0; first < count; ++first) for (PeerIndex second = first + 1; second < count; ++second) {
            const auto& key = pairs[first * count + second];
            if (std::all_of(key.begin(), key.end(), [](auto byte) { return byte == 0; }))
                return fail<ReconnectTokens>("EAWR-NET-1021", "OS reconnect key is zero.");
            for (PeerIndex previous = 0; previous <= first; ++previous)
                for (PeerIndex other = previous + 1; other < count; ++other)
                    if ((previous < first || other < second) && pairs[previous * count + other] == key)
                        return fail<ReconnectTokens>("EAWR-NET-1021", "OS reconnect keys are not distinct.");
        }
        tokens_.assign(pairs.begin(), pairs.begin() + count);
        acknowledged_.assign(transport.peer_count(), false); acknowledged_[0] = true;
        for (PeerIndex peer = 1; peer < transport.peer_count(); ++peer) {
            if (transport.link(peer) != LinkState::open) { clear(); return fail<ReconnectTokens>("EAWR-NET-1022", "Credential delivery requires an admitted seat stream."); }
            Bytes frame; frame.reserve(42 + (count - 1) * 32U);
            EraseFrame eraser{frame};
            frame.insert(frame.end(), {lan_credentials_kind, 1}); append(frame, peer);
            frame.insert(frame.end(), identity_.begin(), identity_.end()); append(frame, count - 1);
            for (PeerIndex remote = 0; remote < count; ++remote) if (remote != peer) {
                const auto& key = pairs[peer * count + remote]; frame.insert(frame.end(), key.begin(), key.end());
            }
            auto sent = transport.send(peer, frame);
            if (!sent) { clear(); return fail<ReconnectTokens>("EAWR-NET-1022", "Reconnect credential delivery failed."); }
        }
        received_ = verified_ = true;
    }
    if (!received_ || identity_ != identity || destination_ != transport.self() || tokens_.size() != offer.seats.size())
        return fail<ReconnectTokens>("EAWR-NET-1023", "Reconnect credentials do not belong to this match and seat.");
    if (!verified_) {
        Bytes ack{lan_credentials_ack_kind}; append(ack, transport.self());
        ack.insert(ack.end(), identity_.begin(), identity_.end());
        auto sent = transport.send(0, ack);
        if (!sent) return fail<ReconnectTokens>("EAWR-NET-1022", "Reconnect credential acknowledgement failed.");
        verified_ = true;
    }
    return core::Result<ReconnectTokens>::success(tokens_);
}
core::Result<bool> LanReconnectCredentials::receive(Received& received, MessageTransport& transport) {
    auto& frame = received.frame;
    if (frame.empty() || (frame[0] != lan_credentials_kind && frame[0] != lan_credentials_ack_kind))
        return core::Result<bool>::success(false);
    EraseFrame eraser{frame};
    if (frame[0] == lan_credentials_ack_kind) {
        if (transport.self() != 0 || !verified_ || frame.size() != 37 || received.from == 0
            || received.from >= acknowledged_.size() || read(std::span(frame).subspan(1)) != received.from
            || !std::equal(identity_.begin(), identity_.end(), frame.begin() + 5))
            return fail<bool>("EAWR-NET-1024", "Invalid reconnect credential acknowledgement.");
        acknowledged_[received.from] = true;
        return core::Result<bool>::success(true);
    }
    if (transport.self() == 0 || received.from != 0 || received_ || frame.size() < 42 || frame[1] != 1
        || read(std::span(frame).subspan(2)) != transport.self())
        return fail<bool>("EAWR-NET-1024", "Invalid reconnect credential delivery authority or seat.");
    const auto count = read(std::span(frame).subspan(38));
    if (transport.peer_count() < 2 || transport.peer_count() > max_match_seats
        || count != transport.peer_count() - 1 || frame.size() != 42 + count * 32U)
        return fail<bool>("EAWR-NET-1024", "Invalid reconnect credential delivery size.");
    tokens_.resize(transport.peer_count());
    std::size_t offset = 42;
    for (PeerIndex remote = 0; remote < transport.peer_count(); ++remote) if (remote != transport.self()) {
        std::copy_n(frame.begin() + static_cast<std::ptrdiff_t>(offset), 32, tokens_[remote].begin()); offset += 32;
    }
    if (!valid_reconnect_tokens(tokens_, transport.self(), transport.peer_count())) { clear(); return fail<bool>("EAWR-NET-1024", "Invalid reconnect credential roster."); }
    std::copy_n(frame.begin() + 6, 32, identity_.begin());
    destination_ = transport.self(); received_ = true;
    return core::Result<bool>::success(true);
}
} // namespace eawr::net
