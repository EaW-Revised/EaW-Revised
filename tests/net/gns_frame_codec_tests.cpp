#include "../support/check.hpp"
#include "../support/result_require.hpp"
#include "../../src/net/gns_frame_codec.hpp"
#include "../../src/net/gns_log_redact.hpp"
#include "../../src/net/gns_room_signal.hpp"
#include "eawr/net/gns_transport.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace {
using namespace eawr::net;
using Bytes = std::vector<std::uint8_t>;
void expect(bool ok, const char* reason) {
    if (!ok) eawr::testing::require(ok, reason);
}
using eawr::testing::result_code_colon::require;
void u32(Bytes& bytes, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}
TcpOptions options() {
    TcpOptions config; config.build_identity = "codec-build"; config.max_frame_size = 64;
    config.queue_bytes = 128; config.queue_frames = 2; return config;
}
Bytes hello(TcpOptions config = options(), std::uint8_t role = 1, std::uint32_t slot = 1, std::uint32_t peers = 2) {
    Bytes bytes{1, 'E', 'A', 'W', 'R'}; u32(bytes, config.protocol_version);
    u32(bytes, static_cast<std::uint32_t>(config.build_identity.size()));
    bytes.insert(bytes.end(), config.build_identity.begin(), config.build_identity.end());
    u32(bytes, config.max_frame_size); u32(bytes, slot); u32(bytes, peers); bytes.push_back(role); return bytes;
}
// The hello as shipped before the roster fields (hello revision 1): no sender slot, no roster size.
Bytes legacy_hello(TcpOptions config = options(), std::uint8_t role = 1) {
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
    // The sender's roster seat and the roster size are checked before any application frame.
    bad(hello(options(), 1, 2), "EAWR-NET-1766", {}); bad(hello(options(), 1, 0), "EAWR-NET-1766", {});
    bad(hello(options(), 1, 1, 3), "EAWR-NET-1766", {}); bad(hello(options(), 1, 1, 9), "EAWR-NET-1766", {});
    bad({2, 2, 0, 0, 0}, "EAWR-NET-1766", {true, false, false, 0, 0});
    { // the dialing side expects the listener's seat, and its own seat in the accept
        GnsFrameDecoder dialer(options(), false);
        expect(require(dialer.receive(hello(options(), 0, 0), {})).kind == TcpMessage::hello, "Dialer accepts the listener's hello");
        const auto wrong = GnsFrameDecoder(options(), false).receive(hello(options(), 0, 1), {});
        expect(!wrong && wrong.error().code == "EAWR-NET-1766", "Dialer refuses a hello from its own seat");
        const auto accepted = dialer.receive(std::array<std::uint8_t, 5>{2, 1, 0, 0, 0}, {true, false, false, 0, 0});
        expect(accepted && accepted.value().kind == TcpMessage::accept, "Dialer accepts its own seat");
        const auto other = GnsFrameDecoder(options(), false).receive(std::array<std::uint8_t, 5>{2, 0, 0, 0, 0}, {true, false, false, 0, 0});
        expect(!other && other.error().code == "EAWR-NET-1766", "Dialer refuses another seat in accept");
    }
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
// The GNS hello revision was bumped on purpose when the roster fields joined it: an old-layout peer (or a new-layout one
// with an older version number) gets the typed protocol mismatch, and a same-revision peer on another build gets the
// build mismatch, whichever side decodes.
void hello_revisions() {
    expect(gns_protocol_version != TcpOptions{}.protocol_version && GnsOptions{}.protocol_version == gns_protocol_version,
        "GNS carries its own hello revision, distinct from the TCP/LAN protocol");
    auto current = options(); current.protocol_version = gns_protocol_version; // what a GNS build sends and expects
    auto legacy = options();  legacy.protocol_version = 1;
    const auto listener_sees = [&](const Bytes& packet, const char* code, TcpOptions config = options(), bool hosting = true) {
        GnsFrameDecoder decoder(config, hosting);
        const auto result = decoder.receive(packet, {});
        expect(!result && result.error().code == code, "Hello revision or build mismatch did not give the typed code");
    };
    const auto& expected = current;
    // Old peer -> new host (and the dialing direction): version first, never 1518.
    listener_sees(legacy_hello(legacy), "EAWR-NET-1502", expected);
    listener_sees(legacy_hello(legacy, 0), "EAWR-NET-1502", expected, false);
    // An old-layout hello that is also a different build still reports the version.
    auto legacy_other = legacy; legacy_other.build_identity += "-other";
    listener_sees(legacy_hello(legacy_other), "EAWR-NET-1502", expected);
    // New peer -> a decoder that still expects revision 1: its version check comes before the length check.
    listener_sees(hello(current), "EAWR-NET-1502", legacy);
    listener_sees(hello(current, 0, 0), "EAWR-NET-1502", legacy, false);
    // Same revision, different build: 1503 in both layouts of the comparison.
    auto other_build = current; other_build.build_identity += "-other";
    listener_sees(hello(other_build), "EAWR-NET-1503", expected);
    listener_sees(hello(other_build, 0, 0), "EAWR-NET-1503", expected, false);
    // Same revision and build is admitted.
    expect(static_cast<bool>(GnsFrameDecoder(expected, true).receive(hello(current), {})), "Matching hello revision and build is admitted");
    // A revision-2 hello with the revision-1 length is malformed, not a mismatch.
    auto truncated = hello(current); truncated.resize(truncated.size() - 8);
    listener_sees(truncated, "EAWR-NET-1518", expected);
    // A non-host pair of a room mesh compares builds as well (the adapter only reaches it after the host pair, which
    // already agreed, but the codec must not rely on that).
    auto mesh = expected; mesh.peers = 4;
    GnsFrameDecoder pair(mesh, true); pair.bind(2, 1);
    auto mesh_other = other_build; mesh_other.peers = 4;
    const auto refused = pair.receive(hello(mesh_other, 1, 1, 4), {});
    expect(!refused && refused.error().code == "EAWR-NET-1503", "A non-host pair refuses a different build");
    auto mesh_current = current; mesh_current.peers = 4;
    expect(static_cast<bool>(pair.receive(hello(mesh_current, 1, 1, 4), {})), "A non-host pair admits the same build and seats");
    GnsFrameDecoder other_pair(mesh, false); other_pair.bind(1, 2);
    const auto dialer = other_pair.receive(hello(mesh_other, 0, 2, 4), {});
    expect(!dialer && dialer.error().code == "EAWR-NET-1503", "A non-host pair dialer refuses a different build too");
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
    // The listener may also report the mesh pair reasons 1766..1771 before admission, and nothing between.
    for (unsigned code = 1766; code <= 1771; ++code) {
        const auto typed = client.receive(wire("EAWR-NET-" + std::to_string(code), "mesh reason"), {});
        expect(!typed && typed.error().code == "EAWR-NET-" + std::to_string(code), "Mesh refusal reason is typed");
    }
    malformed(wire("EAWR-NET-1765", "room fixture range")); malformed(wire("EAWR-NET-1772", "next slice range"));
    malformed(wire("EAWR-NET-15x7", "bad code"));
    malformed(wire("EAWR-NET-1517", std::string(1025, 'x')));
    malformed(packet, open); malformed(packet, {false, false, true, 0, 0}); malformed(packet, {}, false);
    bad(packet, "EAWR-NET-1518", {}); // a joiner cannot refuse a host
}
Bytes control(std::uint8_t kind, Bytes body) { body.insert(body.begin(), kind); return body; }
Bytes words(std::initializer_list<std::uint32_t> values, std::initializer_list<std::uint8_t> tail = {}) {
    Bytes bytes;
    for (const auto value : values) u32(bytes, value);
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}
void room_signal_attempts() {
    GnsFormationSecret old_grant{}, current_grant{};
    old_grant.fill(11); current_grant.fill(29);
    const Bytes request{1, 2, 3, 4}; // opaque native request/proof signalling
    const auto old_wire = encode_room_signal(request, old_grant);
    const auto current_wire = encode_room_signal(request, current_grant);
    const auto old = decode_room_signal(old_wire), current = decode_room_signal(current_wire);
    expect(old && current && room_signal_matches_grant(*old, old_grant), "Original grant carries its native signals");
    // Attempt N's delayed request arrives after the acceptor has installed N+1.
    // It must never enter GNS to create a new link with N+1's expected proof.
    expect(!room_signal_matches_grant(*old, current_grant)
        && room_signal_matches_grant(*current, current_grant), "Regrant rejects the old request and accepts its own request");
    expect(Bytes(current->payload.begin(), current->payload.end()) == request, "Only the opaque native payload reaches GNS");
    for (std::size_t size = 0; size <= old_wire.size() - request.size(); ++size)
        expect(!decode_room_signal(std::span(old_wire).first(size)), "Truncated grant envelope is rejected");
    auto malformed = current_wire; malformed[3] = 255;
    expect(!decode_room_signal(malformed) && !decode_room_signal(request), "Unknown revisions and unwrapped pair signals are rejected");
    expect(encode_room_signal(Bytes(64 * 1024, 7), current_grant).empty(), "The envelope preserves the room signal size bound");
}
void mesh_controls() {
    const Bytes secret(16, 7);
    const auto pair = [&](std::uint32_t from, std::uint32_t to) {
        Bytes body = secret; u32(body, from); u32(body, to); return control(12, body);
    };
    const auto grant = [&](std::uint32_t remote, std::uint8_t dial) {
        Bytes body = words({remote, 3}, {dial}); body.insert(body.end(), secret.begin(), secret.end()); return control(15, body);
    };
    const auto vacated = [&](std::uint32_t seat, std::uint8_t reason, std::uint32_t other, std::uint32_t code = 0, std::uint8_t relay = 0) {
        Bytes body = words({seat, 2}, {reason}); u32(body, other); u32(body, code); body.push_back(relay); return control(16, body);
    };
    const auto report = [&](std::uint32_t remote, std::uint8_t status, std::uint8_t relay, std::uint32_t code = 1762) {
        Bytes body = words({remote}, {status, relay}); u32(body, code);
        body.insert(body.end(), gns_pair_secret_size, 73); return control(17, body);
    };
    const auto ready = [&](std::uint32_t mask) { return control(18, words({mask})); };
    const auto decode = [&](GnsFrameDecoder& decoder, const Bytes& packet, GnsDecodeContext context = {true, true, false, 0, 0}) {
        return decoder.receive(packet, context);
    };
    const auto refused = [&](GnsFrameDecoder& decoder, const Bytes& packet, const char* code,
                             GnsDecodeContext context = {true, true, false, 0, 0}) {
        const auto result = decode(decoder, packet, context);
        expect(!result && result.error().code == code, "Mesh control did not fail closed with the expected code");
    };
    TcpOptions config = options(); config.peers = 4;
    // The two-peer adapter has no mesh controls at all.
    for (const auto kind : {12, 15, 16, 17, 18}) {
        GnsFrameDecoder plain(options(), true);
        refused(plain, control(static_cast<std::uint8_t>(kind), Bytes(16, 0)), "EAWR-NET-1518");
    }
    GnsFrameDecoder member(config, true); member.bind(2, 1); // seat 2 listening for seat 1: a non-host pair
    const auto good = decode(member, pair(1, 2));
    expect(good && good.value().kind == TcpMessage::mesh_pair && good.value().control && good.value().control->size() == 24,
        "Pair proof carries its validated body");
    refused(member, pair(2, 1), "EAWR-NET-1770"); refused(member, pair(1, 3), "EAWR-NET-1770");
    refused(member, control(12, Bytes(23, 0)), "EAWR-NET-1770");
    refused(member, pair(1, 2), "EAWR-NET-1518", {true, false, false, 0, 0});   // before admission
    refused(member, pair(1, 2), "EAWR-NET-1518", {true, true, true, 0, 0});     // after close
    for (const auto kind : {15, 16, 17, 18}) refused(member, control(static_cast<std::uint8_t>(kind), Bytes(25, 0)), "EAWR-NET-1770");
    GnsFrameDecoder host_pair(config, true); host_pair.bind(2, 0); // we are seat 2, the remote is the host
    refused(host_pair, pair(0, 2), "EAWR-NET-1770"); // a host pair has no secret
    expect(decode(host_pair, grant(3, 1)) && decode(host_pair, grant(1, 0)), "Grants from the host");
    refused(host_pair, grant(2, 1), "EAWR-NET-1770"); refused(host_pair, grant(0, 1), "EAWR-NET-1770");
    refused(host_pair, grant(4, 1), "EAWR-NET-1770"); refused(host_pair, grant(3, 2), "EAWR-NET-1770");
    refused(host_pair, control(15, Bytes(24, 0)), "EAWR-NET-1770");
    expect(decode(host_pair, vacated(1, 0, 0)) && decode(host_pair, vacated(3, 1, 1, 1762, 0)) && decode(host_pair, vacated(2, 2, 0)),
        "Vacancies from the host");
    // The refusal carries the failing pair's own reason and relay flag.
    for (const auto code : {1500U, 1507U, 1529U, 1762U, 1766U, 1768U, 1769U, 1771U})
        expect(static_cast<bool>(decode(host_pair, vacated(3, 1, 1, code, 1))), "A refusal carries the failing pair's reason");
    for (const auto code : {0U, 1499U, 1530U, 1760U, 1761U, 1763U, 1765U, 1772U, 9999U})
        refused(host_pair, vacated(3, 1, 1, code, 0), "EAWR-NET-1770");
    refused(host_pair, vacated(3, 1, 1, 1762, 2), "EAWR-NET-1770");
    refused(host_pair, vacated(1, 0, 0, 1762, 0), "EAWR-NET-1770"); refused(host_pair, vacated(1, 2, 0, 0, 1), "EAWR-NET-1770");
    refused(host_pair, control(16, Bytes(13, 0)), "EAWR-NET-1770"); // the previous layout
    refused(host_pair, vacated(0, 0, 0), "EAWR-NET-1770"); refused(host_pair, vacated(4, 0, 0), "EAWR-NET-1770");
    refused(host_pair, vacated(1, 3, 0), "EAWR-NET-1770"); refused(host_pair, vacated(1, 1, 1), "EAWR-NET-1770");
    refused(host_pair, vacated(1, 1, 4), "EAWR-NET-1770"); refused(host_pair, vacated(1, 0, 2), "EAWR-NET-1770");
    refused(host_pair, report(1, 1, 1), "EAWR-NET-1770"); refused(host_pair, ready(2), "EAWR-NET-1770"); // member-to-host only
    GnsFrameDecoder at_host(config, true); at_host.bind(0, 2); // we are the host, the remote is seat 2
    expect(decode(at_host, report(1, 0, 0)) && decode(at_host, report(3, 1, 1)) && decode(at_host, report(1, 2, 0)), "Reports to the host");
    refused(at_host, report(0, 1, 1), "EAWR-NET-1770"); refused(at_host, report(2, 1, 1), "EAWR-NET-1770");
    refused(at_host, report(4, 1, 1), "EAWR-NET-1770"); refused(at_host, report(1, 3, 0), "EAWR-NET-1770");
    refused(at_host, report(1, 1, 2), "EAWR-NET-1770");
    for (const auto code : {1500U, 1507U, 1762U, 1768U, 1771U}) expect(static_cast<bool>(decode(at_host, report(1, 1, 1, code))), "Reports carry the pair's reason");
    for (const auto code : {0U, 1530U, 1760U, 1765U, 1772U}) refused(at_host, report(1, 1, 1, code), "EAWR-NET-1770");
    refused(at_host, control(17, Bytes(10, 0)), "EAWR-NET-1770"); // report without its attempt secret
    auto truncated_report = report(1, 1, 1); truncated_report.pop_back();
    refused(at_host, truncated_report, "EAWR-NET-1770");
    expect(decode(at_host, ready(0b1011)) && decode(at_host, ready(0)), "Ready masks to the host");
    refused(at_host, ready(0b0100), "EAWR-NET-1770"); refused(at_host, ready(0b10000), "EAWR-NET-1770");
    refused(at_host, control(18, Bytes(5, 0)), "EAWR-NET-1770");
    refused(at_host, grant(1, 1), "EAWR-NET-1770"); refused(at_host, vacated(1, 0, 0), "EAWR-NET-1770");
    // A control cannot interrupt a fragmented application frame.
    GnsFrameDecoder split(config, true); split.bind(0, 2);
    expect(static_cast<bool>(decode(split, data(4, 0, {1, 2}))), "Partial frame");
    refused(split, ready(0), "EAWR-NET-1518");
}
}
// #4021: the diagnostic log never carries an address or a raw control character.
static void log_redaction() {
    expect(redact_log_text("ICE: TURN server 203.0.113.7:3478 answered 438") == "ICE: TURN server <addr> answered 438", "A public IPv4 with a port");
    expect(redact_log_text("from 10.1.2.3 and 172.16.0.9:80.") == "from <addr> and <addr>.", "Private IPv4, with and without a port");
    expect(redact_log_text("peer [2001:db8::1]:3478 gone") == "peer <addr> gone", "A bracketed IPv6 with a port");
    expect(redact_log_text("peer [fe80::1] gone") == "peer <addr> gone", "A bracketed IPv6");
    expect(redact_log_text("tick 18603 of 1.2 s, version 1.2.3") == "tick 18603 of 1.2 s, version 1.2.3", "Numbers that are not addresses stay");
    expect(redact_log_text("got 'a\001b'") == "got 'a?b'", "Control characters are replaced");
}
int main() {
    try { log_redaction(); handshake(); hello_revisions(); framing(); refusals(); room_signal_attempts(); mesh_controls(); std::cout << "GNS-independent envelope failure contracts passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
