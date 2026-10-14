#include "eawr/net/transport.hpp"
#include <type_traits>
#include "fake_transport.hpp"
#include <iostream>

static_assert(std::has_virtual_destructor_v<eawr::net::MessageTransport>);
int main() {
    using namespace eawr::testing;
    for (const auto& profile : wan_profiles) {
        FakeNetwork network(2); network.emulate(profile);
        FakeTransport left(network, 0), right(network, 1);
        if (left.link_stats(1)) return 1; // Adapters default to unknown.
        for (std::uint8_t sequence = 0; sequence < 80; ++sequence) {
            if (!left.send(1, std::span<const std::uint8_t>(&sequence, 1))) return 2;
            if (!right.send(0, std::span<const std::uint8_t>(&sequence, 1))) return 3;
        }
        unsigned forward = 0, reverse = 0;
        for (network.round = 0; network.round < 2000; ++network.round) {
            while (auto frame = right.poll()) if (frame->frame != std::vector<std::uint8_t>{static_cast<std::uint8_t>(forward++)}) return 4;
            while (auto frame = left.poll()) if (frame->frame != std::vector<std::uint8_t>{static_cast<std::uint8_t>(reverse++)}) return 5;
        }
        if (forward != 80 || reverse != 80) return 6;
    }
    // Explicit asymmetric delays and recovered loss must block later frames.
    FakeNetwork network(2);
    network.emulate({"asymmetric", {10, 0, 1, 50, 0, 0}, {2, 0, 0, 0, 0, 0}});
    FakeTransport left(network, 0), right(network, 1);
    const std::uint8_t byte = 1;
    if (!left.send(1, {&byte, 1}) || !right.send(0, {&byte, 1})) return 7;
    network.round = 3;
    if (!left.poll() || right.poll()) return 8;
    network.round = 60;
    if (right.poll()) return 9;
    network.round = 61;
    if (!right.poll()) return 10;
    std::cout << "Reliable WAN transport profiles passed\n";
    return 0;
}
