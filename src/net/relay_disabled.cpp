#include "eawr/net/network_introduction.hpp"

namespace eawr::net {
#if !defined(EAWR_HAS_RELAY) || !EAWR_HAS_RELAY
bool NetworkGnsIntroduction::available() { return false; }
NetworkGnsIntroduction::NetworkGnsIntroduction(NetworkIntroductionOptions options) : options_(std::move(options)) {}
static core::Diagnostic relay_disabled() {
    core::Diagnostic result; result.code = "EAWR-NET-1519"; result.message = "Relay client was disabled at build time"; return result;
}
core::Result<GnsIntroduction> NetworkGnsIntroduction::publish(std::string, std::chrono::milliseconds) {
    return core::Result<GnsIntroduction>::failure(relay_disabled());
}
core::Result<std::shared_ptr<GnsSignallingEndpoint>> NetworkGnsIntroduction::join(std::string, std::string) {
    return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(relay_disabled());
}
core::Result<void> NetworkGnsIntroduction::health(NetworkIntroductionOptions) {
    return core::Result<void>::failure(relay_disabled());
}

struct NetworkOnlineLobby::Impl {};
NetworkOnlineLobby::NetworkOnlineLobby(NetworkIntroductionOptions, std::string) : impl_(std::make_unique<Impl>()) {}
NetworkOnlineLobby::~NetworkOnlineLobby() = default;
std::string NetworkOnlineLobby::principal() const { return {}; }
core::Result<OnlineRoomPage> NetworkOnlineLobby::list(std::string, std::uint32_t) { return core::Result<OnlineRoomPage>::failure(relay_disabled()); }
core::Result<OnlineRoomPublication> NetworkOnlineLobby::publish(OnlineRoomMetadata, std::string) { return core::Result<OnlineRoomPublication>::failure(relay_disabled()); }
core::Result<OnlineRoomPublication> NetworkOnlineLobby::publish_bridge(OnlineRoomMetadata, std::string, std::string, std::string) { return core::Result<OnlineRoomPublication>::failure(relay_disabled()); }
core::Result<void> NetworkOnlineLobby::heartbeat(const OnlineRoomPublication&, const OnlineRoomMetadata&) { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkOnlineLobby::remove(const OnlineRoomPublication&) { return core::Result<void>::failure(relay_disabled()); }
core::Result<OnlineRoomAdmission> NetworkOnlineLobby::join(std::string, std::string, std::string) { return core::Result<OnlineRoomAdmission>::failure(relay_disabled()); }
core::Result<void> NetworkOnlineLobby::leave() { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkOnlineLobby::send_chat(std::string) { return core::Result<void>::failure(relay_disabled()); }
core::Result<OnlineChatPage> NetworkOnlineLobby::chat(std::uint32_t) { return core::Result<OnlineChatPage>::failure(relay_disabled()); }
core::Result<void> NetworkOnlineLobby::kick(const OnlineRoomPublication&, std::uint32_t) { return core::Result<void>::failure(relay_disabled()); }

struct NetworkGnsRoom::Impl {};
NetworkGnsRoom::NetworkGnsRoom(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NetworkGnsRoom::~NetworkGnsRoom() = default;
bool NetworkGnsRoom::available() { return false; }
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::connect(NetworkIntroductionOptions, std::vector<std::uint8_t>, std::string, std::optional<std::uint32_t>) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(relay_disabled());
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::open(NetworkIntroductionOptions, const OnlineRoomPublication&, std::string, std::string, bool) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(relay_disabled());
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::attach(NetworkIntroductionOptions, const OnlineRoomAdmission&, std::string) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(relay_disabled());
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::attach_alias(NetworkIntroductionOptions, std::string, std::string) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(relay_disabled());
}
core::Result<std::shared_ptr<NetworkGnsRoom>> NetworkGnsRoom::reattach(NetworkIntroductionOptions, std::string, std::string, std::string) {
    return core::Result<std::shared_ptr<NetworkGnsRoom>>::failure(relay_disabled());
}
std::uint64_t NetworkGnsRoom::dropped_signals() const { return 0; }
std::string NetworkGnsRoom::last_refusal() const { return {}; }
std::string NetworkGnsRoom::alias() const { return {}; }
std::string NetworkGnsRoom::capability() const { return {}; }
PeerIndex NetworkGnsRoom::local_slot() const { return 0; }
GnsRoomRoster NetworkGnsRoom::roster() const { return {}; }
core::Result<void> NetworkGnsRoom::send(PeerIndex, std::span<const std::uint8_t>) { return core::Result<void>::failure(relay_disabled()); }
core::Result<std::optional<GnsRoomSignal>> NetworkGnsRoom::poll() { return core::Result<std::optional<GnsRoomSignal>>::failure(relay_disabled()); }
std::optional<GnsTurnCredentials> NetworkGnsRoom::turn_credentials() const { return {}; }
GnsRoomLease NetworkGnsRoom::lease() const { return {}; }
core::Result<void> NetworkGnsRoom::status() const { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkGnsRoom::leave() { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkGnsRoom::release(PeerIndex) { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkGnsRoom::kick(PeerIndex) { return core::Result<void>::failure(relay_disabled()); }
core::Result<void> NetworkGnsRoom::lock() { return core::Result<void>::failure(relay_disabled()); }
#endif
} // namespace eawr::net
