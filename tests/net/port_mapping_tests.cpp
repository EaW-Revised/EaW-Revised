#include "../support/check.hpp"
#include "eawr/net/port_mapping.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
namespace m = eawr::net::mapping;
using eawr::core::Result;
std::atomic_int failures{};
void expect(bool ok, const char* message) {
    if (!ok) {
        eawr::testing::Checker local;
        local.check(false, message, std::cerr);
        failures += local.failures;
    }
}
template<class T> Result<T> unavailable() {
    eawr::core::Diagnostic d; d.code = "EAWR-NET-1107"; d.message = "Fake router timeout";
    return Result<T>::failure(std::move(d));
}
constexpr m::Route route{0xC0A80101U, 0xC0A80102U};
constexpr std::uint16_t port = 47624;
constexpr m::Nonce nonce{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
// Synthetic recorded router payloads, in network byte order; no live router needed.
const m::Bytes address_reply{0, 128, 0, 0, 0, 0, 0, 9, 203, 0, 113, 7};
const m::Bytes map_reply{0, 130, 0, 0, 0, 0, 0, 9, 186, 8, 156, 64, 0, 0, 0, 120};
std::string response(std::string_view action, std::string_view content = {}) {
    return "<s:Envelope xmlns:s='http://schemas.xmlsoap.org/soap/envelope/'><s:Body><u:"
        + std::string(action) + "Response xmlns:u='urn:schemas-upnp-org:service:WANIPConnection:1'>"
        + std::string(content) + "</u:" + std::string(action) + "Response></s:Body></s:Envelope>";
}
const std::string description = "<root xmlns='urn:schemas-upnp-org:device-1-0'><device><serviceList><service>"
    "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/control</controlURL>"
    "</service></serviceList></device></root>";
struct Retirement {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{}, release{}, retired{}, early_replacement{};
};
class FakeIo final : public m::RouterIo {
public:
    m::Protocol supported{m::Protocol::pcp};
    bool private_address{}, refuse{}, fail_renewal{}, no_route{}, occupied{}, mapped{};
    std::string mapped_description;
    std::string mapped_client;
    m::Route current_route{::route};
    std::shared_ptr<Retirement> retirement;
    bool replacement{};
    std::vector<m::Bytes> requests;
    std::vector<std::string> actions, bodies;
    Result<m::Route> route() override {
        if (retirement && replacement) {
            const std::lock_guard lock(retirement->mutex);
            if (!retirement->retired) retirement->early_replacement = true;
        }
        return no_route ? unavailable<m::Route>() : Result<m::Route>::success(current_route);
    }
    Result<m::Bytes> udp(std::uint32_t address, std::uint16_t destination, std::span<const std::uint8_t> packet,
        std::stop_token stop, bool discovery) override {
        expect(address == ::route.gateway || address == current_route.gateway, "I/O targets the selected or retiring gateway");
        if (stop.stop_requested()) return unavailable<m::Bytes>();
        if (retirement && !replacement && packet.size() == 12 && packet[1] == 2 && packet[11] == 0) {
            std::unique_lock lock(retirement->mutex); retirement->entered = true; retirement->changed.notify_all();
            expect(retirement->changed.wait_for(lock, std::chrono::seconds(2), [&] { return retirement->release; }), "fake old-router cleanup released");
            retirement->retired = true;
        }
        requests.emplace_back(packet.begin(), packet.end());
        if (discovery) {
            expect(destination == 1900, "SSDP destination");
            const std::string reply = "HTTP/1.1 200 OK\r\nLOCATION: http://192.168.1.1:1234/device.xml\r\n\r\n";
            return Result<m::Bytes>::success({reply.begin(), reply.end()});
        }
        expect(destination == 5351, "PCP and NAT-PMP destination");
        if (packet[0] == 2 && supported == m::Protocol::pcp) {
            if (refuse || (fail_renewal && requests.size() > 1 && packet[7])) return unavailable<m::Bytes>();
            auto b = m::Bytes(packet.begin(), packet.end()); b[1] = 129;
            std::fill(b.begin() + 8, b.begin() + 24, std::uint8_t{0}); b[42] = 156; b[43] = 64;
            b[54] = b[55] = 255; b[56] = private_address ? 100 : 203; b[57] = private_address ? 64 : 0; b[58] = 113; b[59] = 7;
            return Result<m::Bytes>::success(std::move(b));
        }
        if (packet[0] == 0 && supported == m::Protocol::nat_pmp) {
            if (packet[1] == 0) { auto b = address_reply; if (private_address) { b[8] = 100; b[9] = 64; } return Result<m::Bytes>::success(std::move(b)); }
            auto b = map_reply; if (!packet[11]) std::fill(b.begin() + 12, b.end(), std::uint8_t{0});
            return Result<m::Bytes>::success(std::move(b));
        }
        return unavailable<m::Bytes>();
    }
    Result<std::string> http(const m::Url& url, std::string_view action, std::string_view body, std::stop_token stop) override {
        expect(url.address == ::route.gateway || url.address == current_route.gateway, "HTTP stays at the selected or retiring gateway");
        if (stop.stop_requested()) return unavailable<std::string>();
        actions.emplace_back(action); bodies.emplace_back(body);
        if (action.empty()) return Result<std::string>::success(description);
        if (action.ends_with("GetSpecificPortMappingEntry")) return Result<std::string>::success(occupied
            ? response("GetSpecificPortMappingEntry") : (mapped ? response("GetSpecificPortMappingEntry",
                "<NewInternalClient>" + mapped_client + "</NewInternalClient><NewInternalPort>47624</NewInternalPort><NewPortMappingDescription>" + mapped_description + "</NewPortMappingDescription>")
                : "<Envelope><Body><Fault><detail><errorCode>714</errorCode></detail></Fault></Body></Envelope>"));
        if (action.ends_with("GetExternalIPAddress")) return Result<std::string>::success(response("GetExternalIPAddress",
            private_address ? "<NewExternalIPAddress>100.64.1.2</NewExternalIPAddress>" : "<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>"));
        if (refuse) return Result<std::string>::success("<Envelope><Body><Fault><detail><errorCode>725</errorCode></detail></Fault></Body></Envelope>");
        if (action.ends_with("AddPortMapping")) {
            mapped = true;
            constexpr std::string_view opening = "<NewPortMappingDescription>";
            const auto begin = body.find(opening) + opening.size(); const auto end = body.find("</NewPortMappingDescription>", begin);
            mapped_description = std::string(body.substr(begin, end - begin));
            constexpr std::string_view client_opening = "<NewInternalClient>";
            const auto client_begin = body.find(client_opening) + client_opening.size();
            const auto client_end = body.find("</NewInternalClient>", client_begin);
            mapped_client = std::string(body.substr(client_begin, client_end - client_begin));
        }
        if (action.ends_with("DeletePortMapping")) mapped = false;
        return Result<std::string>::success(response(action.substr(action.find('#') + 1)));
    }
};
void codecs() {
    const m::Bytes expected{0, 2, 0, 0, 186, 8, 186, 8, 0, 0, 0, 120};
    expect(m::pmp_request(port, port, 120) == expected, "NAT-PMP TCP request bytes");
    const auto deletion = m::pmp_request(port, 40000, 0);
    expect(deletion[6] == 0 && deletion[7] == 0 && deletion[11] == 0, "NAT-PMP deletion clears external port and lifetime");
    auto address = m::pmp_address(address_reply); expect(address && address.value() == 0xCB007107U, "NAT-PMP external address reply");
    auto mapped = m::pmp_reply(map_reply, port);
    expect(mapped && mapped.value().port == 40000 && mapped.value().seconds == 120, "NAT-PMP router-assigned port and lease");
    for (std::size_t size = 0; size < map_reply.size(); ++size) expect(!m::pmp_reply(std::span(map_reply).first(size), port), "truncated PMP mapping rejected");
    auto bad = map_reply; bad[1] = 129; expect(!m::pmp_reply(bad, port), "UDP response cannot satisfy TCP request");
    bad = map_reply; bad[3] = 2; expect(!m::pmp_reply(bad, port), "PMP refusal rejected");
    expect(!m::pmp_reply(map_reply, 123), "PMP wrong internal port rejected");
    const auto request = m::pcp_request(route, nonce, port, 40000, 120);
    expect(request.size() == 60 && request[0] == 2 && request[1] == 1 && request[36] == 6
        && request[18] == 255 && request[20] == 192 && request[23] == 2
        && std::equal(nonce.begin(), nonce.end(), request.begin() + 24), "PCP request header, mapped IPv4 client, TCP and nonce");
    auto reply = request; reply[1] = 129; std::fill(reply.begin() + 8, reply.begin() + 24, std::uint8_t{0});
    reply[54] = reply[55] = 255; reply[56] = 203; reply[57] = 0; reply[58] = 113; reply[59] = 7;
    auto pcp = m::pcp_reply(reply, nonce, port); expect(pcp && pcp.value().port == 40000 && pcp.value().seconds == 120, "PCP recorded mapping reply");
    for (std::size_t size = 0; size < reply.size(); ++size) expect(!m::pcp_reply(std::span(reply).first(size), nonce, port), "truncated PCP rejected");
    auto wrong = nonce; ++wrong[0]; expect(!m::pcp_reply(reply, wrong, port), "PCP nonce correlation");
    expect(!m::pcp_reply(reply, nonce, 123), "PCP internal port correlation");
    reply[3] = 2; expect(!m::pcp_reply(reply, nonce, port), "PCP router error");
    for (const auto ip : {0U, 0x0A000001U, 0x64400001U, 0x647FFFFFU, 0xAC100001U, 0xAC1FFFFFU, 0xC0A80101U, 0x7F000001U, 0xA9FE0001U, 0xE0000001U})
        expect(!m::public_address(ip), "private, shared, loopback and multicast endpoints rejected");
    expect(m::public_address(0xCB007107U) && m::public_address(0x64800001U), "non-private address and CGNAT boundary");
    expect(!m::parse_ipv4("1.2.3.256") && !m::parse_ipv4("1.2.3") && !m::parse_ipv4("1.2.3.4:80"), "invalid address rejected");
}
void upnp() {
    auto location = m::ssdp_location("HTTP/1.1 200 OK\r\nlOcAtIoN: http://192.168.1.1:1234/path/device.xml\r\n\r\n", route.gateway);
    expect(location && location.value().port == 1234, "case-insensitive SSDP location");
    expect(!m::router_url("http://203.0.113.7/control", route.gateway) && !m::router_url("http://192.168.1.1/x\r\nInjected: y", route.gateway), "foreign gateway and header injection rejected");
    expect(!m::ssdp_location("HTTP/1.1 404 Nope\r\nLOCATION: http://192.168.1.1/x\r\n", route.gateway), "SSDP refusal rejected");
    auto service = m::igd_service(description, location.value(), route.gateway);
    expect(service && service.value().control.path == "/control" && service.value().control.port == 1234, "IGD connection service and root-relative URL");
    auto relative = description; relative.replace(relative.find("/control"), 8, "control");
    auto relative_service = m::igd_service(relative, location.value(), route.gateway);
    expect(relative_service && relative_service.value().control.path == "/path/control", "relative control URL resolved from description");
    expect(!m::igd_service("<invalid>", location.value(), route.gateway), "invalid XML rejected");
    std::string nested;
    for (unsigned i = 0; i < 7000; ++i) nested += "<x>";
    nested += "<Fault/>";
    for (unsigned i = 0; i < 7000; ++i) nested += "</x>";
    expect(!m::soap_ok(nested, "AddPortMapping"), "deep router XML fails safely without recursive traversal");
    const auto body = m::soap_body(service.value(), "AddPortMapping", route, port, 40000, 120);
    expect(body.find("<NewProtocol>TCP</NewProtocol>") != body.npos && body.find("<NewExternalPort>40000</NewExternalPort>") != body.npos
        && body.find("<NewInternalClient>192.168.1.2</NewInternalClient>") != body.npos
        && body.find("<NewLeaseDuration>120</NewLeaseDuration>") != body.npos, "SOAP TCP mapping with finite lease");
    const auto deleted = m::soap_body(service.value(), "DeletePortMapping", route, port, 40000, 0);
    expect(deleted.find("<NewExternalPort>40000</NewExternalPort>") != deleted.npos && deleted.find("NewInternalClient") == deleted.npos, "SOAP removal uses assigned external TCP port");
    expect(static_cast<bool>(m::soap_ok(response("AddPortMapping"), "AddPortMapping")), "SOAP namespaced success");
    expect(!m::soap_ok(response("DeletePortMapping"), "AddPortMapping") && !m::soap_ok("<Envelope><Fault/></Envelope>", "AddPortMapping"), "SOAP wrong action and fault rejected");
    expect(m::soap_missing_mapping("<Envelope><Fault><detail><errorCode>714</errorCode></detail></Fault></Envelope>")
        && !m::soap_missing_mapping("<Envelope><Fault><detail><errorCode>725</errorCode></detail></Fault></Envelope>"), "UPnP missing-entry fault distinguished from lease refusal");
    auto ip = m::soap_address(response("GetExternalIPAddress", "<NewExternalIPAddress>203.0.113.7</NewExternalIPAddress>"));
    expect(ip && ip.value() == 0xCB007107U, "SOAP external IPv4 address");
}
void lifecycle() {
    for (auto protocol : {m::Protocol::pcp, m::Protocol::nat_pmp, m::Protocol::upnp}) {
        FakeIo io; io.supported = protocol;
        m::Controller controller(io, nonce, port); auto lease = controller.acquire();
        expect(lease && lease.value().protocol == protocol, "fallback selects supported router protocol");
        expect(lease && lease.value().seconds == 120 && lease.value().port == (protocol == m::Protocol::upnp ? port : 40000), "granted endpoint exposed");
        expect(static_cast<bool>(controller.refresh()), "active lease renewal"); controller.remove();
        if (protocol == m::Protocol::upnp) {
            expect(io.actions.back().ends_with("DeletePortMapping"), "UPnP cleanup action");
        } else {
            const auto lifetime_offset = protocol == m::Protocol::pcp ? 4U : 8U;
            const auto& deletion = io.requests.back();
            expect(std::all_of(deletion.begin() + lifetime_offset, deletion.begin() + lifetime_offset + 4,
                [](auto byte) { return byte == 0; }), "PCP/PMP cleanup lifetime zero");
            if (protocol == m::Protocol::pcp) expect(std::equal(nonce.begin(), nonce.end(), io.requests.back().begin() + 24), "cleanup retains PCP nonce");
        }
        const auto calls = io.actions.size() + io.requests.size(); controller.remove();
        expect(io.actions.size() + io.requests.size() == calls, "cleanup idempotent");
    }
    FakeIo io; io.private_address = true; m::Controller controller(io, nonce, port); auto lease = controller.acquire();
    expect(!lease && lease.error().code == "EAWR-NET-1105" && io.requests.back()[7] == 0, "CGNAT produces nonfatal reason and unmaps");
    FakeIo upnp_io; upnp_io.supported = m::Protocol::upnp; upnp_io.private_address = true;
    m::Controller upnp_controller(upnp_io, nonce, port); lease = upnp_controller.acquire();
    expect(!lease && upnp_io.actions.back().ends_with("GetExternalIPAddress"), "CGNAT UPnP does not create a mapping");
    for (auto protocol : {m::Protocol::nat_pmp, m::Protocol::upnp}) {
        FakeIo changed; changed.supported = protocol; m::Controller current(changed, nonce, port);
        expect(static_cast<bool>(current.acquire()), "mapping before WAN address change"); changed.private_address = true;
        expect(!current.refresh(), "renewal re-reads WAN address and rejects a changed private address");
    }
    FakeIo occupied; occupied.supported = m::Protocol::upnp; occupied.occupied = true;
    m::Controller collision(occupied, nonce, port); lease = collision.acquire();
    expect(!lease && lease.error().code == "EAWR-NET-1108" && occupied.actions.back().ends_with("GetSpecificPortMappingEntry"), "UPnP cannot overwrite or remove someone else's mapping");
    FakeIo replaced; replaced.supported = m::Protocol::upnp; m::Controller owner(replaced, nonce, port);
    expect(static_cast<bool>(owner.acquire()), "UPnP initial ownership"); replaced.occupied = true; owner.remove();
    expect(replaced.actions.back().ends_with("GetSpecificPortMappingEntry"), "UPnP cleanup does not remove a replaced entry");
    FakeIo reused; reused.supported = m::Protocol::upnp;
    m::Controller old_match(reused, nonce, port); expect(static_cast<bool>(old_match.acquire()), "old UPnP match mapping");
    reused.mapped = false; // old lease expired, then another match reused the same client and TCP port
    auto next_nonce = nonce; ++next_nonce[0]; m::Controller next_match(reused, next_nonce, port);
    expect(static_cast<bool>(next_match.acquire()), "replacement UPnP match mapping");
    old_match.remove(); expect(reused.mapped, "old UPnP cleanup cannot remove another match with the same client/port");
    next_match.remove(); expect(!reused.mapped, "replacement UPnP match removes its own entry");
    FakeIo dhcp; dhcp.supported = m::Protocol::upnp;
    m::Controller moving(dhcp, nonce, port); expect(static_cast<bool>(moving.acquire()), "UPnP mapping before DHCP change");
    const auto before_move = dhcp.actions.size(); dhcp.current_route.client = 0xC0A80103U;
    auto remapped = moving.refresh();
    expect(remapped && dhcp.mapped_client == "192.168.1.3", "DHCP refresh remaps to the newly resolved LAN address");
    const auto deleted = std::find_if(dhcp.actions.begin() + static_cast<std::ptrdiff_t>(before_move), dhcp.actions.end(),
        [](const auto& action) { return action.ends_with("DeletePortMapping"); });
    expect(deleted != dhcp.actions.end(), "DHCP remap deletes the old UPnP entry before adding the new one");
    const auto new_add = std::find_if(deleted == dhcp.actions.end() ? dhcp.actions.end() : deleted + 1, dhcp.actions.end(),
        [](const auto& action) { return action.ends_with("AddPortMapping"); });
    expect(new_add != dhcp.actions.end() && dhcp.bodies.back().find("192.168.1.2") == std::string::npos, "DHCP renewal never reasserts the old client");
    FakeIo gateway; m::Controller changing_gateway(gateway, nonce, port);
    expect(static_cast<bool>(changing_gateway.acquire()), "PCP mapping before gateway change");
    gateway.current_route.gateway = 0xC0A801FEU;
    expect(static_cast<bool>(changing_gateway.refresh()), "refresh rediscovers a changed gateway");
    FakeIo pmp_dhcp; pmp_dhcp.supported = m::Protocol::nat_pmp; m::Controller changing_pmp(pmp_dhcp, nonce, port);
    expect(static_cast<bool>(changing_pmp.acquire()), "NAT-PMP mapping before DHCP change");
    const auto pmp_before = pmp_dhcp.requests.size(); pmp_dhcp.current_route.client = 0xC0A80103U;
    expect(static_cast<bool>(changing_pmp.refresh()), "NAT-PMP remaps from the new source IP");
    expect(std::none_of(pmp_dhcp.requests.begin() + static_cast<std::ptrdiff_t>(pmp_before), pmp_dhcp.requests.end(),
        [](const auto& packet) { return packet.size() == 12 && packet[1] == 2 && packet[11] == 0; }),
        "NAT-PMP does not send an old-client deletion from a newly owned IP");
    FakeIo failed; failed.fail_renewal = true; m::Controller renew(failed, nonce, port);
    expect(static_cast<bool>(renew.acquire()), "initial mapping before renewal failure");
    auto renewal = renew.refresh(); expect(!renewal && renewal.error().code == "EAWR-NET-1106" && failed.requests.back()[7] == 0, "renewal failure invalidates endpoint and cleans up");
    FakeIo cancelled; std::stop_source stop; stop.request_stop(); m::Controller cancel(cancelled, nonce, port);
    expect(!cancel.acquire(stop.get_token()) && cancelled.requests.empty(), "cancelled discovery stops protocol fallback");
    FakeIo missing; missing.no_route = true; m::Controller no_route(missing, nonce, port);
    expect(!no_route.acquire() && missing.requests.empty(), "no route does not send packets");
    auto unavailable_io = std::make_unique<FakeIo>(); unavailable_io->no_route = true;
    eawr::net::PortMapping worker(port, std::move(unavailable_io));
    for (unsigned i = 0; i < 100 && worker.view().phase == eawr::net::MappingPhase::discovering; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    expect(worker.view().phase == eawr::net::MappingPhase::unavailable && worker.view().endpoint.empty(), "worker publishes nonfatal mapping failure");
}
void worker_retirement() {
    const auto wait_mapped = [](const eawr::net::PortMapping& worker) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (worker.view().phase == eawr::net::MappingPhase::discovering && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        expect(worker.view().phase == eawr::net::MappingPhase::mapped, "fake mapping worker reached mapped state");
    };
    auto retirement = std::make_shared<Retirement>();
    auto first_io = std::make_unique<FakeIo>(); first_io->supported = m::Protocol::nat_pmp; first_io->retirement = retirement;
    auto first = std::make_unique<eawr::net::PortMapping>(port, std::move(first_io)); wait_mapped(*first);
    auto next_io = std::make_unique<FakeIo>(); next_io->supported = m::Protocol::nat_pmp; next_io->retirement = retirement; next_io->replacement = true;
    eawr::net::PortMapping next(port, std::move(next_io));
    std::jthread retiring([first = std::move(first)]() mutable { first.reset(); });
    {
        std::unique_lock lock(retirement->mutex);
        expect(retirement->changed.wait_for(lock, std::chrono::seconds(2), [&] { return retirement->entered; }), "old mapping cleanup entered");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    expect(next.view().phase == eawr::net::MappingPhase::discovering, "replacement waits for old NAT-PMP cleanup off the caller thread");
    {
        const std::lock_guard lock(retirement->mutex);
        retirement->release = true; retirement->changed.notify_all();
    }
    retiring.join(); wait_mapped(next);
    {
        const std::lock_guard lock(retirement->mutex);
        expect(!retirement->early_replacement, "new router requests follow old cleanup completion");
    }
}
}
int main() { codecs(); upnp(); lifecycle(); worker_retirement(); if (failures) return 1; std::cout << "Router mapping contracts passed\n"; }
