#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::online_test {
enum class Role { host, join };
enum class ParseError { none, argument, config, config_too_large, address, trust };
inline constexpr std::size_t max_config_bytes = 4096;

// How the TLS introduction service is reached and trusted: a certificate pin or the system CA store.
struct ServerConfig {
    std::string address;
    std::uint16_t port{27020};
    std::string certificate_name; // defaults to the address when empty
    std::string certificate_sha256; // 64 lowercase hex digits; empty selects the system CA
    std::string ca_file; // loopback tests only; the package never writes it
};

struct Options {
    Role role{Role::host};
    ServerConfig server;
    std::string code; // join only; prompted for when empty
    std::filesystem::path config_path;
    std::filesystem::path log_path;
    bool turn_only{};
    std::chrono::seconds timeout{90};
};

template<class T> struct Parsed {
    std::optional<T> value;
    std::string error;
    ParseError kind{ParseError::none};
    explicit operator bool() const { return value.has_value(); }
};

// server.conf: `key=value` lines, '#' comments. Keys: address, port, certificate_name,
// trust (`system-ca` or `pin:<64 hex>`), ca_file (tests only).
[[nodiscard]] Parsed<ServerConfig> parse_server_conf(std::string_view text);
// argv[1] is the role; then --config <file>, --code <XXXX>, --log <file>, --address,
// --port, --certificate-name, --pin <hex>, --system-ca, --ca-file, --turn-only, --timeout <s>.
// Without --address/--ca-file the server comes from --config, else server.conf next to the
// executable. Explicit flags override the file.
[[nodiscard]] Parsed<Options> parse_arguments(const std::vector<std::string>& arguments,
    const std::filesystem::path& executable_directory);
[[nodiscard]] bool valid_invitation_code(std::string_view code);
[[nodiscard]] std::string normalise_invitation_code(std::string_view code);
[[nodiscard]] std::string usage();
}
