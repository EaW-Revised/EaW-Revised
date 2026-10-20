#include "../support/check.hpp"
#include "log.hpp"
#include "options.hpp"
#include "gns_link_stats.hpp"

#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

using namespace eawr::online_test;
namespace {
eawr::testing::Checker local_checker;
int& failures = local_checker.failures;
void expect(bool value, const char* what) {
    if (!value) local_checker.check(value, what, std::cerr, "FAILED: ");
}
const std::string pin(64, 'a');

void conf_tests() {
    auto pinned = parse_server_conf("# comment\naddress = play.example.org\nport=27021\ntrust=pin:" + pin + "\n");
    expect(pinned && pinned.value->address == "play.example.org" && pinned.value->port == 27021
        && pinned.value->certificate_sha256 == pin && pinned.value->certificate_name == "play.example.org", "pinned server.conf");
    auto system = parse_server_conf("address=198.51.100.7\ncertificate_name=play.example.org\ntrust=system-ca\n");
    expect(system && system.value->certificate_sha256.empty() && system.value->certificate_name == "play.example.org"
        && system.value->port == 27020, "system CA server.conf with default port");
    auto upper = parse_server_conf("ADDRESS=x\nTrust=PIN:" + std::string(64, 'F') + "\n");
    expect(upper && upper.value->certificate_sha256 == std::string(64, 'f'), "keys and pin are case-insensitive");
    expect(!parse_server_conf("port=27020\ntrust=system-ca\n"), "address is required");
    expect(!parse_server_conf("address=x\n"), "trust is required");
    expect(!parse_server_conf("address=x\ntrust=pin:abc\n"), "short pin refused");
    expect(!parse_server_conf("address=x\ntrust=whatever\n"), "unknown trust refused");
    expect(!parse_server_conf("address=x\ntrust=system-ca\nport=0\n"), "port zero refused");
    expect(!parse_server_conf("address=x\ntrust=system-ca\nport=70000\n"), "port above 65535 refused");
    expect(!parse_server_conf("address=x\ntrust=system-ca\nbogus=1\n"), "unknown key refused");
    expect(!parse_server_conf("address=x\ntrust=system-ca\nnonsense\n"), "line without equals refused");
    auto error = parse_server_conf("address=x\ntrust=system-ca\nport=zz\n");
    expect(!error && error.error.find("line 3") != std::string::npos, "error names the line");
}

void argument_tests(const std::filesystem::path& directory) {
    const auto conf = directory / "server.conf";
    { std::ofstream(conf) << "address=play.example.org\nport=27020\ntrust=system-ca\n"; }
    auto host = parse_arguments({"host"}, directory);
    expect(host && host.value->role == Role::host && host.value->server.address == "play.example.org"
        && host.value->log_path == directory / "online-test-host.log" && host.value->config_path == conf, "host reads server.conf next to the exe");
    auto join = parse_arguments({"join", "--code", "ab3d"}, directory);
    expect(join && join.value->role == Role::join && join.value->code == "AB3D"
        && join.value->log_path.filename() == "online-test-join.log", "join code is normalised");
    auto overridden = parse_arguments({"join", "--config", conf.string(), "--port", "1234", "--pin", pin, "--turn-only", "--timeout", "30",
        "--log", (directory / "x.log").string()}, directory);
    expect(overridden && overridden.value->server.port == 1234 && overridden.value->server.certificate_sha256 == pin
        && overridden.value->turn_only && overridden.value->timeout.count() == 30 && overridden.value->log_path.filename() == "x.log",
        "flags override the file");
    auto inline_server = parse_arguments({"host", "--address", "127.0.0.1", "--port", "9", "--ca-file", "ca.pem", "--certificate-name", "localhost"}, directory / "missing");
    expect(inline_server && inline_server.value->server.ca_file == "ca.pem" && inline_server.value->server.certificate_name == "localhost"
        && inline_server.value->config_path.empty(), "inline server needs no server.conf");
    auto defaulted = parse_arguments({"host", "--address", "play.example.org", "--system-ca"}, directory / "missing");
    expect(defaulted && defaulted.value->server.certificate_name == "play.example.org", "certificate name defaults to the address");
    expect(!parse_arguments({}, directory), "role required");
    expect(!parse_arguments({"watch"}, directory), "unknown role refused");
    expect(!parse_arguments({"host", "--code", "ABCD"}, directory), "host refuses --code");
    expect(!parse_arguments({"join", "--code", "ABCDE"}, directory), "long code refused");
    expect(!parse_arguments({"join", "--code", "A-CD"}, directory), "punctuation in code refused");
    expect(!parse_arguments({"host", "--port"}, directory), "missing value refused");
    expect(!parse_arguments({"host", "--port", "99999"}, directory), "bad port refused");
    expect(!parse_arguments({"host", "--pin", "zz"}, directory), "bad pin refused");
    expect(!parse_arguments({"host", "--pin", pin, "--system-ca"}, directory), "pin and system CA exclude each other");
    expect(!parse_arguments({"host", "--timeout", "0"}, directory), "zero timeout refused");
    expect(!parse_arguments({"host", "--frobnicate", "1"}, directory), "unknown option refused");
    expect(!parse_arguments({"host", "--address", "x"}, directory / "missing"), "inline server without trust needs a server.conf");
    auto trust_from_file = parse_arguments({"host", "--config", conf.string(), "--address", "other.example.org"}, directory);
    expect(trust_from_file && trust_from_file.value->server.address == "other.example.org" && trust_from_file.value->server.certificate_name == "other.example.org" && trust_from_file.value->server.certificate_sha256.empty(),
        "address override keeps the file trust");
    expect(!parse_arguments({"host"}, directory / "missing"), "missing server.conf refused");
    for (const auto* bad : {"bad host", "-bad.example", "bad-.example", "a..b", "999.1.2.3", "host:27020", "bad/host", "bad\nhost"}) {
        const auto parsed = parse_arguments({"host", "--address", bad, "--pin", pin}, directory);
        expect(!parsed && parsed.kind == ParseError::address, "malformed address is typed");
    }
    const auto embedded_nul = parse_arguments({"host", "--address", std::string("127.0.0.1\0evil", 14), "--pin", pin}, directory);
    expect(!embedded_nul && embedded_nul.kind == ParseError::address, "numeric IP with embedded NUL refused");
    expect(!parse_server_conf("address=" + std::string(64, 'a') + ".org\ntrust=system-ca\n"), "DNS label length limited to 63");
    expect(!parse_server_conf("address=" + std::string(254, 'a') + "\ntrust=system-ca\n"), "DNS total length limited to 253");
    auto oversized = parse_server_conf(std::string(max_config_bytes + 1, '#'));
    expect(!oversized && oversized.kind == ParseError::config_too_large, "config text cap is typed");
    { std::ofstream(conf) << std::string(max_config_bytes + 1, '#'); }
    auto oversized_file = parse_arguments({"host"}, directory);
    expect(!oversized_file && oversized_file.kind == ParseError::config_too_large, "config file read is capped");
    expect(!parse_arguments({"host", "--address", "127.0.0.1", "--system-ca"}, directory), "system CA refuses IP name");
    expect(!parse_arguments({"host", "--address", "::1", "--system-ca"}, directory), "system CA refuses IPv6 name");
    expect(static_cast<bool>(parse_arguments({"host", "--address", "::1", "--pin", pin}, directory)), "pin accepts IPv6");
    auto os_store = parse_arguments({"host", "--address", "test.example.org", "--system-ca"}, directory);
    expect(os_store && os_store.value->server.ca_file.empty() && os_store.value->server.certificate_sha256.empty()
        && os_store.value->server.certificate_name == "test.example.org", "system CA passes DNS name with no CA file or pin");
    expect(valid_invitation_code("A1b2") && !valid_invitation_code("A1b") && !valid_invitation_code("A 1b"), "code shape");
}

void sanitize_tests() {
    const std::vector<std::string> secrets{"K7QP", "play.example.org"};
    expect(sanitize("code K7QP at play.example.org", secrets) == "code <redacted> at <redacted>", "secrets redacted");
    expect(sanitize("peer 203.0.113.9:27020 joined", {}) == "peer <address> joined", "IPv4 with port redacted");
    expect(sanitize("local 192.168.1.20 relay 10.0.0.1", {}) == "local <address> relay <address>", "several IPv4 redacted");
    expect(sanitize("peer [2001:db8::1]:27020", {}).find("2001") == std::string::npos, "IPv6 redacted");
    expect(sanitize("peer fe80::1ff:fe23:4567:890a", {}).find("fe80") == std::string::npos, "link-local IPv6 redacted");
    expect(sanitize("tick 10 hash 0123456789abcdef EAWR-NET-1519 build 1.2.3", {}) == "tick 10 hash 0123456789abcdef EAWR-NET-1519 build 1.2.3",
        "ordinary text stays");
    expect(sanitize("hash 0123ABCD4567abcd8901 code ABCD", {"ABCD", "abcd"}) == "hash 0123ABCD4567abcd8901 code <redacted>", "code substrings never corrupt hashes");
    expect(sanitize("turn-user=sample-user password=sample-secret uri=turn:relay.example.org", {"sample-user", "sample-secret", "turn:relay.example.org"})
        == "turn-user=<redacted> password=<redacted> uri=<redacted>", "TURN secrets redacted");
    const auto path = std::filesystem::temp_directory_path() / ("eawr-online-test-log-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".txt");
    {
        Log log(path);
        log.add_secret("ZZ99");
        expect(log.line("code ZZ99 from 198.51.100.4") == "code <redacted> from <address>", "Log returns the sanitized line");
    }
    std::string content;
    {
        std::ifstream stream(path);
        content.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    expect(content.find("ZZ99") == std::string::npos && content.find("198.51") == std::string::npos
        && content.find("<redacted>") != std::string::npos, "log file holds only sanitized text");
    std::filesystem::remove(path);
}

void route_tests() {
    using eawr::net::GnsRoute;
    using eawr::net::classify_gns_route;
    // Source-derived cases for the fields populated by pinned GNS ICE TransportPopulateConnectionInfo.
    // The native selected-pair path sets Relayed and clears remote IP for TURN,
    // and publishes a remote IP for host/reflexive candidates. SDR is not TURN.
    expect(classify_gns_route(true, true, false, false) == GnsRoute::relayed, "selected TURN flags");
    expect(classify_gns_route(true, false, true, false) == GnsRoute::direct, "selected direct ICE address");
    expect(classify_gns_route(true, true, true, false) == GnsRoute::unknown, "conflicting flags are unknown");
    expect(classify_gns_route(true, false, false, false) == GnsRoute::unknown, "missing selected route is unknown");
    expect(classify_gns_route(true, true, false, true) == GnsRoute::unknown, "SDR is not reported as TURN");
    expect(classify_gns_route(false, false, true, false) == GnsRoute::unknown, "disconnected is unknown");
}
}

int main() {
    const auto directory = std::filesystem::temp_directory_path() / ("eawr-online-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    try {
        conf_tests();
        argument_tests(directory);
        sanitize_tests();
        route_tests();
    } catch (const std::exception& error) {
        std::cerr << "FAILED: exception " << error.what() << "\n";
        return 1;
    }
    std::filesystem::remove_all(directory);
    if (failures == 0) std::cout << "online test kit unit contracts passed\n";
    return failures == 0 ? 0 : 1;
}
