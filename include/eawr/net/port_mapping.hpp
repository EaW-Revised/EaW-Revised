#pragma once
#include "eawr/core/result.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::net {
namespace mapping {
using Bytes = std::vector<std::uint8_t>;
using Nonce = std::array<std::uint8_t, 12>;
enum class Protocol { pcp, nat_pmp, upnp };
struct Lease {
    Protocol protocol{Protocol::pcp};
    std::uint32_t address{}, seconds{};
    std::uint16_t port{};
};
struct Route { std::uint32_t gateway{}, client{}; };
struct Url { std::uint32_t address{}; std::uint16_t port{80}; std::string path{"/"}; };
struct Service { Url control; std::string type; };
// RFC 6886 sections 3.2-3.4; RFC 6887 section 11 (TCP MAP).
Bytes pmp_request(std::uint16_t internal, std::uint16_t external, std::uint32_t seconds);
core::Result<std::uint32_t> pmp_address(std::span<const std::uint8_t> reply);
core::Result<Lease> pmp_reply(std::span<const std::uint8_t> reply, std::uint16_t internal);
Bytes pcp_request(Route route, Nonce nonce, std::uint16_t internal, std::uint16_t external,
    std::uint32_t seconds, std::uint32_t address = 0);
core::Result<Lease> pcp_reply(std::span<const std::uint8_t> reply, Nonce nonce, std::uint16_t internal);
std::string ipv4(std::uint32_t address);
core::Result<std::uint32_t> parse_ipv4(std::string_view value);
bool public_address(std::uint32_t address);
core::Result<Url> router_url(std::string_view value, std::uint32_t gateway);
core::Result<Url> ssdp_location(std::string_view reply, std::uint32_t gateway);
core::Result<Service> igd_service(std::string_view xml, Url description, std::uint32_t gateway);
std::string soap_body(const Service& service, std::string_view action, Route route,
    std::uint16_t internal, std::uint16_t external, std::uint32_t seconds);
core::Result<std::uint32_t> soap_address(std::string_view xml);
core::Result<void> soap_ok(std::string_view xml, std::string_view action);
bool soap_missing_mapping(std::string_view xml);
// Injectable bounded I/O: all calls run on the mapping worker, never the sim thread.
class RouterIo {
public:
    virtual ~RouterIo() = default;
    virtual core::Result<Route> route() = 0;
    virtual core::Result<Bytes> udp(std::uint32_t address, std::uint16_t port,
        std::span<const std::uint8_t> request, std::stop_token stop, bool discovery = false) = 0;
    virtual core::Result<std::string> http(const Url& url, std::string_view action,
        std::string_view body, std::stop_token stop) = 0;
};
std::unique_ptr<RouterIo> make_router_io();
// Engine-free synchronous controller for fake-router tests. Its owner is the worker below.
class Controller {
public:
    Controller(RouterIo& io, Nonce nonce, std::uint16_t port);
    ~Controller();
    core::Result<Lease> acquire(std::stop_token stop = {});
    core::Result<Lease> refresh(std::stop_token stop = {});
    void remove(); // best effort; never requests a permanent lease
private:
    bool owns_upnp(std::stop_token stop);
    core::Result<Lease> update(std::uint32_t seconds, std::stop_token stop);
    RouterIo& io_;
    Nonce nonce_;
    std::uint16_t port_;
    Route route_{};
    Service service_{};
    Lease lease_{};
    bool active_{};
};
} // namespace mapping
enum class MappingPhase { disabled, discovering, mapped, unavailable };
struct MappingView {
    MappingPhase phase{MappingPhase::disabled};
    std::string endpoint, code, message;
};
// Opt-in by construction. Destruction stops renewal and removes the owned mapping;
// finite leases also expire after process termination without cleanup.
class PortMapping final {
public:
    explicit PortMapping(std::uint16_t port, std::unique_ptr<mapping::RouterIo> io = {});
    ~PortMapping();
    PortMapping(const PortMapping&) = delete;
    PortMapping& operator=(const PortMapping&) = delete;
    MappingView view() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace eawr::net
