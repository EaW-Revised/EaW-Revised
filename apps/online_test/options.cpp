#include "options.hpp"
#include "platform_socket.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>

namespace eawr::online_test {
namespace {
std::string trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    return std::string(value);
}
bool all_of_class(std::string_view value, int (*test)(int)) {
    return std::all_of(value.begin(), value.end(), [test](char c) { return test(static_cast<unsigned char>(c)) != 0; });
}
bool hex64(std::string_view value) { return value.size() == 64 && all_of_class(value, std::isxdigit); }
std::string lower(std::string value) {
    for (auto& c : value) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return value;
}
bool parse_port(std::string_view text, std::uint16_t& port) {
    if (text.empty() || text.size() > 5 || !all_of_class(text, std::isdigit)) return false;
    const auto value = std::stoul(std::string(text));
    if (value == 0 || value > 65535) return false;
    port = static_cast<std::uint16_t>(value);
    return true;
}
bool valid_address(std::string_view value) {
    if (value.empty() || value.size() > 253
        || std::any_of(value.begin(), value.end(), [](unsigned char c) { return c < 33 || c > 126; })) return false;
    const std::string text(value);
    if (net::socket::numeric_ip(text, false) || net::socket::numeric_ip(text, true)) return true;
    // A malformed numeric literal must not pass as a DNS name.
    if (std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) return false;
    std::size_t start = 0;
    while (start < value.size()) {
        const auto end = value.find('.', start);
        const auto label = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (char c : label)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-')) return false;
        if (end == std::string_view::npos) return true;
        start = end + 1;
    }
    return false;
}
bool system_trust_name(const ServerConfig& config) {
    return !config.certificate_sha256.empty() || !config.ca_file.empty()
        || (!net::socket::numeric_ip(config.certificate_name, false) && !net::socket::numeric_ip(config.certificate_name, true));
}
std::string error_code(ParseError kind) {
    switch (kind) {
    case ParseError::config: return "ONLINE-TEST-CONFIG";
    case ParseError::config_too_large: return "ONLINE-TEST-CONFIG-SIZE";
    case ParseError::address: return "ONLINE-TEST-ADDRESS";
    case ParseError::trust: return "ONLINE-TEST-TRUST";
    default: return "ONLINE-TEST-ARGUMENT";
    }
}
bool set_trust(ServerConfig& config, std::string_view value, std::string& error) {
    const auto text = lower(trim(value));
    if (text == "system-ca") { config.certificate_sha256.clear(); return true; }
    if (text.starts_with("pin:") && hex64(std::string_view(text).substr(4))) { config.certificate_sha256 = text.substr(4); return true; }
    error = "trust must be 'system-ca' or 'pin:' followed by 64 hexadecimal digits";
    return false;
}
Parsed<std::string> read_text(const std::filesystem::path& path) {
    Parsed<std::string> result;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) { result.kind = ParseError::config; result.error = "ONLINE-TEST-CONFIG: cannot read server.conf"; return result; }
    std::string text(max_config_bytes + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (text.size() > max_config_bytes) {
        result.kind = ParseError::config_too_large; result.error = "ONLINE-TEST-CONFIG-SIZE: server.conf exceeds 4096 bytes"; return result;
    }
    result.value = std::move(text);
    return result;
}
} // namespace

Parsed<ServerConfig> parse_server_conf(std::string_view text) {
    ServerConfig config;
    bool trust{};
    std::string line;
    unsigned number{};
    Parsed<ServerConfig> result;
    const auto fail = [&](const std::string& why, ParseError kind = ParseError::config) {
        result.kind = kind;
        result.error = error_code(kind) + ": server.conf line " + std::to_string(number) + ": " + why;
        return result;
    };
    if (text.size() > max_config_bytes) return fail("exceeds 4096 bytes", ParseError::config_too_large);
    std::istringstream lines{std::string(text)};
    while (std::getline(lines, line)) {
        ++number;
        const auto stripped = trim(line);
        if (stripped.empty() || stripped.front() == '#') continue;
        const auto equals = stripped.find('=');
        if (equals == std::string::npos) return fail("expected key=value");
        const auto key = lower(trim(std::string_view(stripped).substr(0, equals)));
        const auto value = trim(std::string_view(stripped).substr(equals + 1));
        std::string error;
        if (key == "address") config.address = value;
        else if (key == "port") { if (!parse_port(value, config.port)) return fail("port must be 1-65535"); }
        else if (key == "certificate_name") config.certificate_name = value;
        else if (key == "ca_file") config.ca_file = value;
        else if (key == "trust") { if (!set_trust(config, value, error)) return fail(error, ParseError::trust); trust = true; }
        else return fail("unknown key '" + key + "'");
    }
    if (!valid_address(config.address)) return fail("address must be an IPv4/IPv6 literal or DNS name", ParseError::address);
    if (!trust && config.ca_file.empty()) return fail("missing trust setting (system-ca or pin:<sha256>)", ParseError::trust);
    if (config.certificate_name.empty()) config.certificate_name = config.address;
    if (!valid_address(config.certificate_name)) return fail("invalid certificate_name", ParseError::address);
    if (!system_trust_name(config)) return fail("system-ca requires a DNS certificate_name; IP literals are refused", ParseError::trust);
    result.value = std::move(config);
    return result;
}

bool valid_invitation_code(std::string_view code) { return code.size() == 4 && all_of_class(code, std::isalnum); }
std::string normalise_invitation_code(std::string_view code) {
    auto text = trim(code);
    for (auto& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}
std::string usage() {
    return "usage: eawr_online_test <host|join> [--config server.conf] [--code XXXX] [--log file]\n"
           "       [--address A --port P (--pin SHA256HEX | --system-ca) [--certificate-name N]]\n"
           "       [--turn-only] [--timeout seconds]\n";
}

Parsed<Options> parse_arguments(const std::vector<std::string>& arguments, const std::filesystem::path& executable_directory) {
    Parsed<Options> result;
    const auto fail = [&](std::string why, ParseError kind = ParseError::argument) { result.kind = kind; result.error = error_code(kind) + ": " + why; return result; };
    if (arguments.empty()) return fail("missing role (host or join)");
    Options options;
    if (arguments[0] == "host") options.role = Role::host;
    else if (arguments[0] == "join") options.role = Role::join;
    else return fail("unknown role '" + arguments[0] + "'");
    ServerConfig given;
    bool address{}, port{}, name{}, ca{}, pin{}, system_ca{};
    std::optional<std::filesystem::path> config;
    for (std::size_t i = 1; i < arguments.size(); ++i) {
        const auto& flag = arguments[i];
        if (flag == "--turn-only") { options.turn_only = true; continue; }
        if (flag == "--system-ca") { system_ca = true; continue; }
        if (i + 1 >= arguments.size()) return fail("missing value for " + flag);
        const auto& value = arguments[++i];
        if (flag == "--config") config = value;
        else if (flag == "--code") options.code = normalise_invitation_code(value);
        else if (flag == "--log") options.log_path = value;
        else if (flag == "--address") { given.address = value; address = true; }
        else if (flag == "--port") { if (!parse_port(value, given.port)) return fail("port must be 1-65535"); port = true; }
        else if (flag == "--certificate-name") { given.certificate_name = value; name = true; }
        else if (flag == "--ca-file") { given.ca_file = value; ca = true; }
        else if (flag == "--pin") {
            given.certificate_sha256 = lower(trim(value));
            if (!hex64(given.certificate_sha256)) return fail("--pin needs 64 hexadecimal digits");
            pin = true;
        } else if (flag == "--timeout") {
            if (value.empty() || value.size() > 4 || !all_of_class(value, std::isdigit) || std::stoul(value) == 0)
                return fail("--timeout needs seconds (1-9999)");
            options.timeout = std::chrono::seconds(std::stoul(value));
        } else return fail("unknown option '" + flag + "'");
    }
    if (pin && system_ca) return fail("--pin and --system-ca exclude each other");
    if (!options.code.empty() && !valid_invitation_code(options.code)) return fail("the invitation code has 4 letters or digits");
    if (options.role == Role::host && !options.code.empty()) return fail("--code is for the join role");
    if (config || !(ca || pin || system_ca)) {
        options.config_path = config ? *config : executable_directory / "server.conf";
        const auto text = read_text(options.config_path);
        if (!text) { result.kind = text.kind; result.error = text.error; return result; }
        auto parsed = parse_server_conf(*text.value);
        if (!parsed) { result.kind = parsed.kind; result.error = parsed.error; return result; }
        options.server = std::move(*parsed.value);
    } else if (!system_ca && !pin && !ca) return fail("give --pin, --system-ca or --config");
    if (address) options.server.address = given.address;
    if (port) options.server.port = given.port;
    if (ca) options.server.ca_file = given.ca_file;
    if (pin) options.server.certificate_sha256 = given.certificate_sha256;
    if (system_ca) { options.server.certificate_sha256.clear(); options.server.ca_file.clear(); }
    if (name) options.server.certificate_name = given.certificate_name;
    else if (address) options.server.certificate_name = options.server.address;
    if (!valid_address(options.server.address)) return fail("address must be an IPv4/IPv6 literal or DNS name", ParseError::address);
    if (options.server.certificate_name.empty()) options.server.certificate_name = options.server.address;
    if (!valid_address(options.server.certificate_name)) return fail("invalid certificate_name", ParseError::address);
    if (!system_trust_name(options.server)) return fail("system-ca requires a DNS certificate_name; IP literals are refused", ParseError::trust);
    if (system_ca && ca) return fail("--system-ca and --ca-file exclude each other", ParseError::trust);
    if (options.log_path.empty())
        options.log_path = executable_directory / (options.role == Role::host ? "online-test-host.log" : "online-test-join.log");
    result.value = std::move(options);
    return result;
}
}
