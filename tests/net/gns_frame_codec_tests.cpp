#include "../../src/net/gns_frame_codec.hpp"

#include <iostream>
#include <array>
#include <stdexcept>

namespace {
using namespace eawr::net;
using Bytes = std::vector<std::uint8_t>;
void expect(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
template<class T> T require(eawr::core::Result<T> value) {
    if (!value) throw std::runtime_error(value.error().code + ": " + value.error().message);
    return std::move(value).value();
}
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
TcpOptions options() {
    TcpOptions config; config.build_identity = "codec-build"; config.max_frame_size = 64;
    config.queue_bytes = 128; config.queue_frames = 2; return config;
}
Bytes hello(TcpOptions config = options(), std::uint8_t role = 1) {
    Bytes bytes{1, 'E', 'A', 'W', 'R'}; u32(bytes, config.protocol_version);
    u32(bytes, static_cast<std::uint32_t>(config.build_identity.size()));
    bytes.insert(bytes.end(), config.build_identity.begin(), config.build_identity.end());
    u32(bytes, config.max_frame_size); bytes.push_back(role); return bytes;
}
Bytes data(std::uint32_t total, std::uint32_t offset, Bytes fragment = {}) {
    Bytes bytes{6}; u32(bytes, total); u32(bytes, offset); bytes.insert(bytes.end(), fragment.begin(), fragment.end()); return bytes;
}
constexpr GnsDecodeContext open{true, true, false, 0, 0};
void bad(Bytes packet, const char* code = "EAWR-NET-1518", GnsDecodeContext context = open, bool reliable = true) {
    GnsFrameDecoder decoder(options(), true);
    const auto result = decoder.receive(packet, context, reliable);
    expect(!result && result.error().code == code, "Malformed GNS envelope did not fail closed with the expected code");
}
void handshake() {
    GnsFrameDecoder decoder(options(), true);
    expect(require(decoder.receive(hello(), {})).kind == TcpMessage::hello, "Valid host-side hello");
    bad(hello()); // duplicate hello after agreement
    auto config = options(); ++config.protocol_version; bad(hello(config), "EAWR-NET-1502", {});
    config = options(); config.build_identity += "-other"; bad(hello(config), "EAWR-NET-1503", {});
    config = options(); ++config.max_frame_size; bad(hello(config), "EAWR-NET-1504", {});
    bad(hello(options(), 0), "EAWR-NET-1518", {});
    auto wire = hello(); wire[1] = 'x'; bad(wire, "EAWR-NET-1518", {});
    wire = hello(); wire.push_back(0); bad(wire, "EAWR-NET-1518", {});
    wire = hello(); wire[9] = 255; wire[10] = 255; bad(wire, "EAWR-NET-1518", {});
    wire = hello();
    for (std::size_t length = 0; length < wire.size(); ++length)
        bad(Bytes(wire.begin(), wire.begin() + static_cast<std::ptrdiff_t>(length)), "EAWR-NET-1518", {});
    expect(require(decoder.receive(std::array<std::uint8_t, 5>{2, 1, 0, 0, 0}, {true, false, false, 0, 0})).kind
        == TcpMessage::accept, "Checked peer assignment");
    bad({2, 1, 0, 0, 0}); bad({2, 1, 0, 0, 0}, "EAWR-NET-1518", {}); bad({2, 2, 0, 0, 0});
}
void framing() {
    bad({}); bad({255}); bad({3}); bad({6}); bad({4, 0}); bad({5, 0});
    bad({4}, "EAWR-NET-1518", {}); bad({5}, "EAWR-NET-1518", {});
    bad(data(0, 0), "EAWR-NET-1518", {});
    bad(data(0, 0), "EAWR-NET-1518", {true, true, true, 0, 0});
    bad({4}, "EAWR-NET-1518", {true, true, true, 0, 0});
    bad(data(65, 0, {1}), "EAWR-NET-1504");
    bad(data(0xffffffffU, 0, {1}), "EAWR-NET-1504");
    bad(data(2, 1, {1})); bad(data(2, 3, {1})); bad(data(2, 0, {})); bad(data(0, 0, {1}));
    bad(data(0, 0), "EAWR-NET-1505", {true, true, false, 0, 2});
    bad(data(64, 0, {1}), "EAWR-NET-1505", {true, true, false, 65, 0});
    bad(data(0, 0), "EAWR-NET-1505", {true, true, false, 129, 0});
    bad(data(0, 0), "EAWR-NET-1518", open, false);
    const Bytes ack_probe{static_cast<std::uint8_t>(TcpMessage::keepalive)};
    bad(ack_probe, "EAWR-NET-1518", {}, false);
    bad({static_cast<std::uint8_t>(TcpMessage::keepalive), 0}, "EAWR-NET-1518", open, false);
    bad({static_cast<std::uint8_t>(TcpMessage::close)}, "EAWR-NET-1518", open, false);
    expect(require(GnsFrameDecoder(options(), true).receive(ack_probe, {true, true, true, 0, 0}, false)).kind
        == TcpMessage::keepalive, "Post-close native ACK probe carries no game frame");
    GnsFrameDecoder decoder(options(), true);
    expect(!require(decoder.receive(data(4, 0, {1, 2}), open)).frame, "Partial frame is not emitted");
    const auto control = decoder.receive(std::array<std::uint8_t, 1>{4}, open);
    expect(!control && control.error().code == "EAWR-NET-1518", "Control cannot split a fragmented frame");
    expect(!require(decoder.receive(ack_probe, open, false)).frame, "Out-of-band native ACK probe cannot alter fragment assembly");
    expect(!decoder.receive(data(4, 0, {3, 4}), open), "Duplicate fragment rejected");
    expect(!decoder.receive(data(5, 2, {3, 4}), open), "Changed frame total rejected");
    const auto complete = require(decoder.receive(data(4, 2, {3, 4}), open));
    expect(complete.frame && *complete.frame == Bytes{1, 2, 3, 4}, "Exactly one whole ordered frame");
    const auto empty = require(decoder.receive(data(0, 0), open));
    expect(empty.frame && empty.frame->empty(), "Empty application frame has an envelope");
    require(decoder.receive(data(4, 0, {1}), open)); decoder.reset();
    expect(require(decoder.receive(data(1, 0, {9}), open)).frame == std::optional<Bytes>{Bytes{9}}, "Reset discards failed attempt assembly");
}
void refusals() {
    const auto wire = [](std::string code, std::string message) {
        Bytes bytes{static_cast<std::uint8_t>(TcpMessage::refuse)};
        u32(bytes, static_cast<std::uint32_t>(code.size())); bytes.insert(bytes.end(), code.begin(), code.end());
        u32(bytes, static_cast<std::uint32_t>(message.size())); bytes.insert(bytes.end(), message.begin(), message.end());
        return bytes;
    };
    const auto packet = wire("EAWR-NET-1517", "GNS introduction service failed");
    GnsFrameDecoder client(options(), false);
    const auto result = client.receive(packet, {});
    expect(!result && result.error().code == "EAWR-NET-1517"
        && result.error().message == "GNS introduction service failed", "Pre-hello host refusal diagnostic");
    expect(!client.receive(packet, {true, false, false, 0, 0}), "Refusal after hello is terminal");
    auto malformed = [&](Bytes bytes, GnsDecodeContext context = {}, bool reliable = true) {
        const auto decoded = client.receive(bytes, context, reliable);
        expect(!decoded && decoded.error().code == "EAWR-NET-1518", "Invalid refusal did not fail closed");
    };
    for (std::size_t size = 0; size < packet.size(); ++size)
        malformed(Bytes(packet.begin(), packet.begin() + static_cast<std::ptrdiff_t>(size)));
    auto extra = packet; extra.push_back(0); malformed(extra);
    malformed(wire("EAWR-NET-1530", "bad range"));
    malformed(wire("EAWR-NET-15x7", "bad code"));
    malformed(wire("EAWR-NET-1517", std::string(1025, 'x')));
    malformed(packet, open); malformed(packet, {false, false, true, 0, 0}); malformed(packet, {}, false);
    bad(packet, "EAWR-NET-1518", {}); // a joiner cannot refuse a host
}
}
int main() {
    try { handshake(); framing(); refusals(); std::cout << "GNS-independent envelope failure contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
