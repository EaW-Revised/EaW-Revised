#include "eawr/net/port_mapping.hpp"
#include <pugixml.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <map>
#include <mutex>
#include <random>
#include <thread>

namespace eawr::net {
namespace mapping {
namespace {
template<class T> core::Result<T> fail(std::string code, std::string message) {
    core::Diagnostic d; d.code = std::move(code); d.message = std::move(message);
    return core::Result<T>::failure(std::move(d));
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t i) {
    return static_cast<std::uint16_t>((static_cast<unsigned>(b[i]) << 8U) | b[i + 1]);
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t i) {
    return (static_cast<std::uint32_t>(u16(b, i)) << 16U) | u16(b, i + 2);
}
void put16(Bytes& b, std::size_t i, std::uint16_t v) {
    b[i] = static_cast<std::uint8_t>(v >> 8U); b[i + 1] = static_cast<std::uint8_t>(v);
}
void put32(Bytes& b, std::size_t i, std::uint32_t v) {
    put16(b, i, static_cast<std::uint16_t>(v >> 16U)); put16(b, i + 2, static_cast<std::uint16_t>(v));
}
std::string_view local_name(const pugi::xml_node& node) {
    std::string_view name(node.name()); const auto colon = name.find(':');
    return colon == name.npos ? name : name.substr(colon + 1);
}
pugi::xml_node find(pugi::xml_node node, std::string_view name) {
    // Router XML is untrusted. Iterate instead of recursing on its nesting depth.
    std::vector<pugi::xml_node> pending{node};
    while (!pending.empty()) {
        const auto next = pending.back(); pending.pop_back();
        if (local_name(next) == name) return next;
        for (auto child : next.children()) pending.push_back(child);
    }
    return {};
}
std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
constexpr std::uint32_t lifetime = 120; // finite, short crash-expiry lease; refresh at half the granted lifetime
std::mutex gates_mutex;
std::map<std::uint16_t, std::weak_ptr<std::timed_mutex>> gates;
std::shared_ptr<std::timed_mutex> worker_gate(std::uint16_t port) {
    const std::lock_guard lock(gates_mutex);
    auto& entry = gates[port];
    auto gate = entry.lock();
    if (!gate) { gate = std::make_shared<std::timed_mutex>(); entry = gate; }
    return gate;
}
std::string owner_description(Nonce nonce) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result = "EAWR-";
    for (auto byte : nonce) { result += digits[byte >> 4U]; result += digits[byte & 15U]; }
    return result;
}
}
std::string ipv4(std::uint32_t a) {
    return std::to_string(a >> 24U) + "." + std::to_string((a >> 16U) & 255U) + "."
        + std::to_string((a >> 8U) & 255U) + "." + std::to_string(a & 255U);
}
core::Result<std::uint32_t> parse_ipv4(std::string_view value) {
    std::uint32_t address = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const auto dot = value.find('.'); const auto part = value.substr(0, dot);
        unsigned octet{}; const auto n = std::from_chars(part.data(), part.data() + part.size(), octet);
        if (part.empty() || n.ec != std::errc{} || n.ptr != part.data() + part.size() || octet > 255
            || (i < 3 && dot == value.npos) || (i == 3 && dot != value.npos))
            return fail<std::uint32_t>("EAWR-NET-1102", "Router returned an invalid IPv4 address.");
        address = (address << 8U) | octet;
        if (dot != value.npos) value.remove_prefix(dot + 1);
    }
    return core::Result<std::uint32_t>::success(address);
}
bool public_address(std::uint32_t a) {
    const auto first = a >> 24U;
    return first != 0 && first != 10 && first != 127 && first < 224
        && (a & 0xFFC00000U) != 0x64400000U // carrier-grade NAT 100.64/10
        && (a & 0xFFFF0000U) != 0xA9FE0000U && (a & 0xFFF00000U) != 0xAC100000U
        && (a & 0xFFFF0000U) != 0xC0A80000U;
}
Bytes pmp_request(std::uint16_t internal, std::uint16_t external, std::uint32_t seconds) {
    Bytes b(12); b[1] = 2; put16(b, 4, internal); put16(b, 6, seconds ? external : 0); put32(b, 8, seconds); return b;
}
core::Result<std::uint32_t> pmp_address(std::span<const std::uint8_t> b) {
    if (b.size() != 12 || b[0] != 0 || b[1] != 128 || u16(b, 2) != 0)
        return fail<std::uint32_t>("EAWR-NET-1102", "NAT-PMP external-address request was refused or malformed.");
    return core::Result<std::uint32_t>::success(u32(b, 8));
}
core::Result<Lease> pmp_reply(std::span<const std::uint8_t> b, std::uint16_t internal) {
    if (b.size() != 16 || b[0] != 0 || b[1] != 130 || u16(b, 2) != 0 || u16(b, 8) != internal)
        return fail<Lease>("EAWR-NET-1102", "NAT-PMP TCP mapping was refused or malformed.");
    return core::Result<Lease>::success({Protocol::nat_pmp, 0, u32(b, 12), u16(b, 10)});
}
Bytes pcp_request(Route route, Nonce nonce, std::uint16_t internal, std::uint16_t external,
    std::uint32_t seconds, std::uint32_t address) {
    Bytes b(60); b[0] = 2; b[1] = 1; put32(b, 4, seconds);
    b[18] = b[19] = 255; put32(b, 20, route.client);
    std::copy(nonce.begin(), nonce.end(), b.begin() + 24); b[36] = 6; // TCP
    put16(b, 40, internal); put16(b, 42, external);
    if (address) { b[54] = b[55] = 255; put32(b, 56, address); }
    return b;
}
core::Result<Lease> pcp_reply(std::span<const std::uint8_t> b, Nonce nonce, std::uint16_t internal) {
    if (b.size() < 60 || b.size() > 1100 || b.size() % 4 != 0 || b[0] != 2 || b[1] != 129 || b[3] != 0
        || !std::equal(nonce.begin(), nonce.end(), b.begin() + 24) || b[36] != 6 || u16(b, 40) != internal
        || !std::all_of(b.begin() + 44, b.begin() + 54, [](auto v) { return v == 0; }) || b[54] != 255 || b[55] != 255)
        return fail<Lease>("EAWR-NET-1102", "PCP TCP mapping was refused, mismatched or is not IPv4.");
    return core::Result<Lease>::success({Protocol::pcp, u32(b, 56), u32(b, 4), u16(b, 42)});
}
core::Result<Url> router_url(std::string_view value, std::uint32_t gateway) {
    if (!value.starts_with("http://") || value.size() > 2048 || value.find_first_of("\r\n\t #") != value.npos)
        return fail<Url>("EAWR-NET-1103", "UPnP returned an unsupported router URL.");
    value.remove_prefix(7); const auto slash = value.find('/'); const auto authority = value.substr(0, slash);
    const auto colon = authority.find(':'); auto address = parse_ipv4(authority.substr(0, colon));
    if (!address || address.value() != gateway) return fail<Url>("EAWR-NET-1103", "UPnP URL is outside the selected gateway.");
    unsigned port = 80;
    if (colon != authority.npos) {
        const auto part = authority.substr(colon + 1); const auto n = std::from_chars(part.data(), part.data() + part.size(), port);
        if (n.ec != std::errc{} || n.ptr != part.data() + part.size() || port == 0 || port > 65535)
            return fail<Url>("EAWR-NET-1103", "UPnP returned an invalid router port.");
    }
    return core::Result<Url>::success({gateway, static_cast<std::uint16_t>(port), slash == value.npos ? "/" : std::string(value.substr(slash))});
}
core::Result<Url> ssdp_location(std::string_view reply, std::uint32_t gateway) {
    if (!reply.starts_with("HTTP/1.1 200") || reply.size() > 8192)
        return fail<Url>("EAWR-NET-1103", "Invalid SSDP response.");
    while (!reply.empty()) {
        const auto end = reply.find("\r\n"); const auto line = reply.substr(0, end); const auto colon = line.find(':');
        auto key = std::string(line.substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (key == "location" && colon != line.npos) return router_url(trim(line.substr(colon + 1)), gateway);
        if (end == reply.npos) break;
        reply.remove_prefix(end + 2);
    }
    return fail<Url>("EAWR-NET-1103", "Gateway did not advertise a UPnP location.");
}
core::Result<Service> igd_service(std::string_view xml, Url description, std::uint32_t gateway) {
    pugi::xml_document doc;
    if (xml.size() > 65536 || !doc.load_buffer(xml.data(), xml.size()))
        return fail<Service>("EAWR-NET-1103", "Invalid UPnP device description.");
    if (auto base = find(doc, "URLBase")) {
        auto parsed = router_url(trim(base.text().get()), gateway);
        if (!parsed) return core::Result<Service>::failure(parsed.error());
        description = parsed.value();
    }
    std::vector<pugi::xml_node> nodes{doc};
    while (!nodes.empty()) {
        const auto node = nodes.back(); nodes.pop_back();
        if (local_name(node) == "service") {
            const auto type = trim(find(node, "serviceType").text().get());
            if (type == "urn:schemas-upnp-org:service:WANIPConnection:1" || type == "urn:schemas-upnp-org:service:WANIPConnection:2"
                || type == "urn:schemas-upnp-org:service:WANPPPConnection:1") {
                const auto control = trim(find(node, "controlURL").text().get());
                if (control.empty()) continue;
                std::string url = control;
                if (!control.starts_with("http://")) {
                    const auto path = control.front() == '/' ? control : description.path.substr(0, description.path.rfind('/') + 1) + control;
                    url = "http://" + ipv4(description.address) + ":" + std::to_string(description.port) + path;
                }
                auto parsed = router_url(url, gateway);
                if (!parsed) return core::Result<Service>::failure(parsed.error());
                return core::Result<Service>::success({parsed.value(), type});
            }
        }
        for (auto child : node.children()) nodes.push_back(child);
    }
    return fail<Service>("EAWR-NET-1103", "Gateway has no supported UPnP connection service.");
}
namespace {
std::string soap_body_impl(const Service& service, std::string_view action, Route route,
    std::uint16_t internal, std::uint16_t external, std::uint32_t seconds, std::string_view description) {
    std::string args;
    if (action == "AddPortMapping" || action == "DeletePortMapping" || action == "GetSpecificPortMappingEntry") {
        args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + std::to_string(external)
            + "</NewExternalPort><NewProtocol>TCP</NewProtocol>";
        if (action == "AddPortMapping") args += "<NewInternalPort>" + std::to_string(internal)
            + "</NewInternalPort><NewInternalClient>" + ipv4(route.client)
            + "</NewInternalClient><NewEnabled>1</NewEnabled><NewPortMappingDescription>" + std::string(description)
            + "</NewPortMappingDescription><NewLeaseDuration>"
            + std::to_string(seconds) + "</NewLeaseDuration>";
    }
    return "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:"
        + std::string(action) + " xmlns:u=\"" + service.type + "\">" + args + "</u:" + std::string(action) + "></s:Body></s:Envelope>";
}
}
std::string soap_body(const Service& service, std::string_view action, Route route,
    std::uint16_t internal, std::uint16_t external, std::uint32_t seconds) {
    return soap_body_impl(service, action, route, internal, external, seconds, "EAWR direct match");
}
core::Result<void> soap_ok(std::string_view xml, std::string_view action) {
    pugi::xml_document doc;
    if (xml.size() > 65536 || !doc.load_buffer(xml.data(), xml.size()) || find(doc, "Fault")
        || !find(doc, std::string(action) + "Response"))
        return fail<void>("EAWR-NET-1104", "UPnP action was refused or returned a malformed response (finite leases required).");
    return core::Result<void>::success();
}
core::Result<std::uint32_t> soap_address(std::string_view xml) {
    auto ok = soap_ok(xml, "GetExternalIPAddress");
    if (!ok) return core::Result<std::uint32_t>::failure(ok.error());
    pugi::xml_document doc; doc.load_buffer(xml.data(), xml.size());
    return parse_ipv4(trim(find(doc, "NewExternalIPAddress").text().get()));
}
bool soap_missing_mapping(std::string_view xml) {
    pugi::xml_document doc;
    if (xml.size() > 65536 || !doc.load_buffer(xml.data(), xml.size())) return false;
    auto fault = find(doc, "Fault");
    return fault && trim(find(fault, "errorCode").text().get()) == "714";
}
Controller::Controller(RouterIo& io, Nonce nonce, std::uint16_t port) : io_(io), nonce_(nonce), port_(port) {}
Controller::~Controller() { try { remove(); } catch (...) { /* Finite lease expires if cleanup I/O fails. */ } }
core::Result<Lease> Controller::acquire(std::stop_token stop) {
    if (!port_) return fail<Lease>("EAWR-NET-1100", "A listening TCP port is required for router mapping.");
    auto route = io_.route(); if (!route) return core::Result<Lease>::failure(route.error()); route_ = route.value();
    auto packet = io_.udp(route_.gateway, 5351, pcp_request(route_, nonce_, port_, port_, lifetime), stop);
    auto result = packet ? pcp_reply(packet.value(), nonce_, port_) : core::Result<Lease>::failure(packet.error());
    if (!result && !stop.stop_requested()) {
        const Bytes address_request{0, 0}; auto address_packet = io_.udp(route_.gateway, 5351, address_request, stop);
        auto address = address_packet ? pmp_address(address_packet.value()) : core::Result<std::uint32_t>::failure(address_packet.error());
        if (address) {
            packet = io_.udp(route_.gateway, 5351, pmp_request(port_, port_, lifetime), stop);
            result = packet ? pmp_reply(packet.value(), port_) : core::Result<Lease>::failure(packet.error());
            if (result) result.value().address = address.value();
        }
    }
    if (!result && !stop.stop_requested()) {
        const std::string search = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\n\r\n";
        packet = io_.udp(route_.gateway, 1900, {reinterpret_cast<const std::uint8_t*>(search.data()), search.size()}, stop, true);
        if (!packet) return fail<Lease>("EAWR-NET-1101", "No router mapping service replied (PCP, NAT-PMP or UPnP may be disabled).");
        const std::string reply(packet.value().begin(), packet.value().end()); auto location = ssdp_location(reply, route_.gateway);
        if (!location) return core::Result<Lease>::failure(location.error());
        auto xml = io_.http(location.value(), {}, {}, stop); if (!xml) return core::Result<Lease>::failure(xml.error());
        auto service = igd_service(xml.value(), location.value(), route_.gateway);
        if (!service) return core::Result<Lease>::failure(service.error());
        service_ = service.value();
        auto external = io_.http(service_.control, service_.type + "#GetExternalIPAddress",
            soap_body(service_, "GetExternalIPAddress", route_, port_, port_, lifetime), stop);
        auto address = external ? soap_address(external.value()) : core::Result<std::uint32_t>::failure(external.error());
        if (!address) return core::Result<Lease>::failure(address.error());
        // Do not open a port on a known upstream private/CGNAT path.
        if (!public_address(address.value())) return fail<Lease>("EAWR-NET-1105", "Gateway reports a private/shared external address: upstream NAT or CGNAT prevents direct Internet hosting.");
        // UPnP AddPortMapping can replace an existing entry. Refuse an occupied port,
        // including entries from earlier crashed instances, rather than taking ownership.
        auto existing = io_.http(service_.control, service_.type + "#GetSpecificPortMappingEntry",
            soap_body(service_, "GetSpecificPortMappingEntry", route_, port_, port_, lifetime), stop);
        if (!existing || !soap_missing_mapping(existing.value()))
            return fail<Lease>("EAWR-NET-1108", "UPnP TCP port is already mapped, or the gateway could not verify that it is free.");
        lease_ = {Protocol::upnp, address.value(), lifetime, port_};
        result = update(lifetime, stop);
    }
    if (!result) return result;
    lease_ = result.value(); active_ = lease_.seconds != 0 && lease_.port != 0;
    if (!active_) return fail<Lease>("EAWR-NET-1102", "Router returned an empty TCP mapping lease.");
    if (!public_address(lease_.address)) {
        remove(); return fail<Lease>("EAWR-NET-1105", "Gateway reports a private/shared external address: upstream NAT or CGNAT prevents direct Internet hosting.");
    }
    return result;
}
core::Result<Lease> Controller::update(std::uint32_t seconds, std::stop_token stop) {
    if (lease_.protocol == Protocol::upnp) {
        if (active_ && !owns_upnp(stop))
            return fail<Lease>("EAWR-NET-1108", "UPnP mapping ownership changed; it will not be renewed or removed.");
        const auto action = seconds ? "AddPortMapping" : "DeletePortMapping";
        auto xml = io_.http(service_.control, service_.type + "#" + action,
            soap_body_impl(service_, action, route_, port_, lease_.port, seconds, owner_description(nonce_)), stop);
        auto ok = xml ? soap_ok(xml.value(), action) : core::Result<void>::failure(xml.error());
        if (!ok) return core::Result<Lease>::failure(ok.error());
        auto next = lease_; next.seconds = seconds;
        if (seconds && active_) {
            auto xml_address = io_.http(service_.control, service_.type + "#GetExternalIPAddress",
                soap_body(service_, "GetExternalIPAddress", route_, port_, lease_.port, seconds), stop);
            auto address = xml_address ? soap_address(xml_address.value()) : core::Result<std::uint32_t>::failure(xml_address.error());
            if (!address) return core::Result<Lease>::failure(address.error());
            next.address = address.value();
        }
        return core::Result<Lease>::success(next);
    }
    auto packet = io_.udp(route_.gateway, 5351, lease_.protocol == Protocol::pcp
        ? pcp_request(route_, nonce_, port_, lease_.port, seconds, lease_.address)
        : pmp_request(port_, lease_.port, seconds), stop);
    if (!packet) return core::Result<Lease>::failure(packet.error());
    auto result = lease_.protocol == Protocol::pcp ? pcp_reply(packet.value(), nonce_, port_) : pmp_reply(packet.value(), port_);
    if (result && lease_.protocol == Protocol::nat_pmp) {
        result.value().address = lease_.address;
        if (seconds) {
            const Bytes address_request{0, 0}; auto packet_address = io_.udp(route_.gateway, 5351, address_request, stop);
            auto address = packet_address ? pmp_address(packet_address.value()) : core::Result<std::uint32_t>::failure(packet_address.error());
            if (!address) return core::Result<Lease>::failure(address.error());
            result.value().address = address.value();
        }
    }
    return result;
}
bool Controller::owns_upnp(std::stop_token stop) {
    auto xml = io_.http(service_.control, service_.type + "#GetSpecificPortMappingEntry",
        soap_body(service_, "GetSpecificPortMappingEntry", route_, port_, lease_.port, lifetime), stop);
    if (!xml || !soap_ok(xml.value(), "GetSpecificPortMappingEntry")) return false;
    pugi::xml_document doc; doc.load_buffer(xml.value().data(), xml.value().size());
    return trim(find(doc, "NewInternalClient").text().get()) == ipv4(route_.client)
        && trim(find(doc, "NewInternalPort").text().get()) == std::to_string(port_)
        && trim(find(doc, "NewPortMappingDescription").text().get()) == owner_description(nonce_);
}
core::Result<Lease> Controller::refresh(std::stop_token stop) {
    if (!active_) return fail<Lease>("EAWR-NET-1106", "There is no active router lease to refresh.");
    auto current_route = io_.route();
    if (!current_route) {
        remove();
        return fail<Lease>("EAWR-NET-1106", "The host's network route is unavailable; the shared endpoint is no longer valid.");
    }
    if (current_route.value().gateway != route_.gateway || current_route.value().client != route_.client) {
        // Never keep forwarding to a DHCP address the host no longer owns.
        // Cleanup uses the old route; acquire discovers the new route/service.
        remove();
        return acquire(stop);
    }
    auto next = update(lifetime, stop);
    if (next && next.value().seconds && next.value().port && public_address(next.value().address)) lease_ = next.value();
    else {
        remove(); return fail<Lease>("EAWR-NET-1106", "Router lease renewal failed; the shared endpoint is no longer available.");
    }
    return next;
}
void Controller::remove() {
    if (!active_) return;
    if (lease_.protocol == Protocol::nat_pmp) {
        const auto current = io_.route();
        // NAT-PMP keys deletion by the UDP source address, not an explicit client
        // or nonce. After DHCP changes, the old lease must expire rather than
        // risk deleting a mapping owned by the host at the newly selected IP.
        if (!current || current.value().client != route_.client || current.value().gateway != route_.gateway) {
            active_ = false; return;
        }
    }
    (void)update(0, {}); active_ = false;
}
std::string mapping_endpoint(const Lease& lease) { return ipv4(lease.address) + ":" + std::to_string(lease.port); }
} // namespace mapping
struct PortMapping::Impl {
    mutable std::mutex mutex;
    MappingView status{MappingPhase::discovering, {}, {}, "Trying router port mapping..."};
    std::unique_ptr<mapping::RouterIo> io;
    std::jthread worker; // destroyed first, while io and status still exist
    Impl(std::uint16_t port, std::unique_ptr<mapping::RouterIo> supplied) : io(supplied ? std::move(supplied) : mapping::make_router_io()) {
        worker = std::jthread([this, port](std::stop_token stop) {
            mapping::Nonce nonce{};
            try {
                // A late NAT-PMP deletion identifies only the client and TCP port.
                // Order its bounded cleanup attempt before a replacement worker;
                // finite expiry remains the fallback if no acknowledgement arrives.
                auto gate = mapping::worker_gate(port);
                std::unique_lock gate_lock(*gate, std::defer_lock);
                while (!stop.stop_requested() && !gate_lock.try_lock_for(std::chrono::milliseconds(20))) {}
                if (stop.stop_requested()) return;
                std::random_device random; for (auto& byte : nonce) byte = static_cast<std::uint8_t>(random());
                mapping::Controller controller(*io, nonce, port);
                auto result = controller.acquire(stop);
                while (result && !stop.stop_requested()) {
                    { const std::lock_guard lock(mutex); status = {MappingPhase::mapped, mapping::mapping_endpoint(result.value()), {},
                        "Share " + mapping::mapping_endpoint(result.value()) + " (router mapping; Internet reachability is not verified)."}; }
                    std::mutex wait_mutex; std::condition_variable_any wake; std::unique_lock lock(wait_mutex);
                    wake.wait_for(lock, stop, std::chrono::seconds(std::max(1U, result.value().seconds / 2)), [] { return false; });
                    if (!stop.stop_requested()) result = controller.refresh(stop);
                }
                controller.remove();
                if (!result && !stop.stop_requested()) {
                    const std::lock_guard lock(mutex); status = {MappingPhase::unavailable, {}, result.error().code, result.error().message};
                }
            } catch (...) {
                const std::lock_guard lock(mutex); status = {MappingPhase::unavailable, {}, "EAWR-NET-1109", "Router mapping worker failed; direct hosting remains available."};
            }
        });
    }
};
PortMapping::PortMapping(std::uint16_t port, std::unique_ptr<mapping::RouterIo> io) : impl_(std::make_unique<Impl>(port, std::move(io))) {}
PortMapping::~PortMapping() = default;
MappingView PortMapping::view() const { const std::lock_guard lock(impl_->mutex); return impl_->status; }
} // namespace eawr::net
