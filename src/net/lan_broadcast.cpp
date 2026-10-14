#include "eawr/net/lan_broadcast.hpp"

#include "platform_socket.hpp"

#include <algorithm>
#include <array>
#include <random>

namespace eawr::net {

struct LanBroadcaster::Impl {
    explicit Impl(LanBroadcastOptions value)
        : options(std::move(value)), directory({0, options.protocol, options.build}) {}
    ~Impl() { socket::close(handle); }

    LanBroadcastOptions options;
    socket::Handle handle{socket::invalid};
    std::uint16_t port{};
    std::uint64_t self{};
    LanDirectory directory;
    LanPresence presence;
    std::optional<LanClock::time_point> last_sent;
    bool dirty{true};
    std::uint32_t chat_sequence{};
    std::uint64_t ignored{};
    std::vector<std::uint16_t> targets;

    void send_everywhere(std::span<const std::uint8_t> bytes) {
        for (const std::uint16_t target : targets) {
            socket::udp_send(handle, socket::ipv4_broadcast, target, bytes);
            socket::udp_send(handle, socket::ipv4_loopback, target, bytes);
        }
    }
};

core::Result<std::unique_ptr<LanBroadcaster>> LanBroadcaster::open(LanBroadcastOptions options) {
    auto impl = std::make_unique<Impl>(std::move(options));
    const socket::Open opened = socket::udp_open(impl->options.bind_address, impl->options.listen_port);
    if (opened.handle == socket::invalid) {
        core::Diagnostic diagnostic;
        diagnostic.code = "EAWR-NET-0701";
        diagnostic.message = "Cannot open the LAN lobby port " + std::to_string(impl->options.listen_port) + ".";
        return core::Result<std::unique_ptr<LanBroadcaster>>::failure(std::move(diagnostic));
    }
    impl->handle = opened.handle;
    impl->port = opened.port;
    std::random_device device;
    do {
        impl->self = (static_cast<std::uint64_t>(device()) << 32) ^ device()
            ^ static_cast<std::uint64_t>(LanClock::now().time_since_epoch().count());
    } while (impl->self == 0);
    impl->directory = LanDirectory({impl->self, impl->options.protocol, impl->options.build});
    impl->targets.push_back(impl->port);
    for (const std::uint16_t extra : impl->options.extra_ports) {
        if (std::find(impl->targets.begin(), impl->targets.end(), extra) == impl->targets.end()) {
            impl->targets.push_back(extra);
        }
    }
    impl->presence.sender = impl->self;
    impl->presence.protocol = impl->options.protocol;
    impl->presence.build = impl->options.build;
    return core::Result<std::unique_ptr<LanBroadcaster>>::success(
        std::unique_ptr<LanBroadcaster>(new LanBroadcaster(std::move(impl))));
}

LanBroadcaster::LanBroadcaster(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
LanBroadcaster::~LanBroadcaster() = default;

std::uint64_t LanBroadcaster::self() const noexcept { return impl_->self; }
std::uint16_t LanBroadcaster::port() const noexcept { return impl_->port; }
LanDirectory& LanBroadcaster::directory() noexcept { return impl_->directory; }
const LanDirectory& LanBroadcaster::directory() const noexcept { return impl_->directory; }
std::uint64_t LanBroadcaster::ignored_datagrams() const noexcept { return impl_->ignored; }

void LanBroadcaster::set_presence(LanPresence presence) {
    presence.sender = impl_->self;
    presence.protocol = impl_->options.protocol;
    presence.build = impl_->options.build;
    if (!(presence == impl_->presence)) {
        impl_->presence = std::move(presence);
        impl_->dirty = true;
    }
}

void LanBroadcaster::send_chat(const ChatScope scope, std::string game, std::string name, std::string text) {
    LanChat chat;
    chat.sender = impl_->self;
    chat.sequence = ++impl_->chat_sequence;
    chat.scope = scope;
    chat.game = std::move(game);
    chat.name = std::move(name);
    chat.text = std::move(text);
    impl_->send_everywhere(encode_chat(chat));
}

bool LanBroadcaster::pump(const LanClock::time_point now) {
    Impl& impl = *impl_;
    if (impl.dirty || !impl.last_sent || now - *impl.last_sent >= lan_presence_interval) {
        impl.send_everywhere(encode_presence(impl.presence));
        impl.last_sent = now;
        impl.dirty = false;
    }
    bool changed = false;
    std::array<std::uint8_t, lan_max_datagram + 1> buffer{};
    // Bounded per call so a flooded port cannot hold the frame.
    for (unsigned count = 0; count < 256; ++count) {
        const socket::Datagram datagram = socket::udp_receive(impl.handle, buffer);
        if (datagram.status != socket::Status::ready) break;
        if (datagram.count == 0) continue;
        const std::span<const std::uint8_t> bytes(buffer.data(), datagram.count);
        if (!decode_lan_message(bytes)) {
            ++impl.ignored;
            continue;
        }
        if (impl.directory.receive(bytes, ipv4_text(datagram.from), now)) changed = true;
    }
    const std::size_t before = impl.directory.players().size();
    impl.directory.prune(now);
    if (impl.directory.players().size() != before) changed = true;
    return changed;
}

} // namespace eawr::net
