#include "relay_rtt.hpp"

#include "platform_socket.hpp"

#include <algorithm>
#include <array>
#include <random>
#include <thread>
#include <utility>
#include <vector>

namespace eawr::net::relay {
namespace {
using Clock = std::chrono::steady_clock;
constexpr unsigned requests = 3;
constexpr auto spacing = std::chrono::milliseconds(10);

// "a.b.c.d" as a host-order integer; false for anything else.
bool ipv4(const std::string& text, std::uint32_t& value) {
    value = 0;
    unsigned parts = 0, part = 0, digits = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '.') {
            if (digits == 0 || part > 255 || ++parts > 4) return false;
            value = (value << 8) | part; part = 0; digits = 0;
        } else if (text[i] >= '0' && text[i] <= '9' && digits < 3) { part = part * 10 + static_cast<unsigned>(text[i] - '0'); ++digits; }
        else return false;
    }
    return parts == 4;
}

struct Pending {
    std::array<std::uint8_t, 12> id{};
    Clock::time_point sent;
    bool answered{};
};
} // namespace

std::optional<std::chrono::microseconds> stun_rtt(const std::string& address, std::uint16_t port, std::chrono::milliseconds cap,
        std::stop_token stop) {
    std::uint32_t target{};
    if (!ipv4(address, target) || port == 0 || cap.count() <= 0) return std::nullopt;
    const auto socket_open = socket::udp_open("0.0.0.0", 0, false);
    if (socket_open.handle == socket::invalid) return std::nullopt;
    struct Closer { socket::Handle handle; ~Closer() { socket::close(handle); } } closer{socket_open.handle};

    std::random_device entropy;
    std::vector<Pending> sent;
    std::vector<std::chrono::microseconds> samples;
    const auto start = Clock::now();
    // `cap` bounds each request's own round trip; the last request goes out 20 ms after the first, so the wait is cap + 20 ms.
    const auto deadline = cap + spacing * static_cast<int>(requests - 1);
    unsigned attempted = 0;
    std::array<std::uint8_t, 1500> buffer{};
    while (!stop.stop_requested()) {
        const auto now = Clock::now();
        if (now - start >= deadline) break;
        if (attempted < requests && now - start >= spacing * static_cast<int>(attempted)) {
            ++attempted;
            Pending request;
            for (auto& byte : request.id) byte = static_cast<std::uint8_t>(entropy());
            std::array<std::uint8_t, 20> packet{0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42};
            std::copy(request.id.begin(), request.id.end(), packet.begin() + 8);
            request.sent = Clock::now();
            if (socket::udp_send(socket_open.handle, target, port, packet)) sent.push_back(request);
            // A failed send is one unanswered probe; replies to the others are still read.
        }
        const auto datagram = socket::udp_receive(socket_open.handle, buffer);
        if (datagram.status == socket::Status::error) break;
        if (datagram.status == socket::Status::ready && datagram.from == target && datagram.count >= 20
            && buffer[0] == 0x01 && buffer[1] == 0x01 && buffer[4] == 0x21 && buffer[5] == 0x12 && buffer[6] == 0xA4 && buffer[7] == 0x42) {
            const auto arrived = Clock::now();
            for (auto& request : sent)
                if (!request.answered && std::equal(request.id.begin(), request.id.end(), buffer.begin() + 8)) {
                    request.answered = true;
                    if (arrived - request.sent > cap) continue; // slower than the cap: not an answer
                    samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(arrived - request.sent));
                }
            if (samples.size() == requests) break;
            continue; // drain: more datagrams may be waiting
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return rtt_from_answers(std::move(samples));
}

std::optional<std::chrono::microseconds> rtt_from_answers(std::vector<std::chrono::microseconds> answers) {
    if (answers.size() < 2) return std::nullopt; // two or more of the three probes unanswered
    std::sort(answers.begin(), answers.end());
    return answers[answers.size() / 2];
}
} // namespace eawr::net::relay
