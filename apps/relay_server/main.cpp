#include "eawr/net/relay_server.hpp"
#include "eawr/core/text.hpp"
#include "eawr/net/network_introduction.hpp"
#include "relay_tls.hpp"

#include <algorithm>
#include <charconv>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping{};
void stop(int) { stopping = 1; }
std::string trim(std::string value) {
    return eawr::core::trim(value, eawr::core::xml_whitespace);
}
std::string normalize(std::string key) { std::replace(key.begin(), key.end(), '-', '_'); return key; }
using Config = std::map<std::string, std::string>;
Config defaults() {
    return {{"listen_address", "127.0.0.1"}, {"port", "27020"}, {"certificate_file", ""}, {"key_file", ""},
        {"access_token", ""}, {"require_access_token", "0"}, {"room_aliases", "0"}, {"failed_guesses_per_minute", "100"}, {"turn_uri", ""}, {"turn_secret", ""},
        {"turn_provider", "coturn"}, {"cloudflare_turn_key_id", ""}, {"cloudflare_turn_api_token", ""},
        {"cloudflare_turn_calls_per_minute", "120"}, {"cloudflare_turn_timeout_ms", "4000"}, {"max_connections", "1024"}, {"max_sessions", "512"},
        {"connections_per_ip", "18"}, {"accepts_per_minute", "30"}, {"operations_per_minute", "120"},
        {"unpaired_sessions_per_ip", "2"},
        {"bytes_per_minute", "1048576"}, {"directory_operations_per_minute", "2048"}, {"directory_bytes_per_minute", "8388608"}, {"menu_operations_per_minute", "1024"}, {"menu_bytes_per_minute", "4194304"}, {"menu_max_members", "512"}, {"queue_bytes", "262144"}, {"queue_frames", "64"},
        {"admission_timeout_ms", "10000"}, {"max_join_lifetime_ms", "900000"}, {"idle_timeout_ms", "60000"},
        {"session_lifetime_ms", "7200000"}, {"max_session_lifetime_ms", "86400000"}, {"renewal_window_ms", "600000"},
        {"room_operations_per_member_per_minute", "480"}, {"room_bytes_per_member_per_minute", "2097152"},
        {"room_reservation_timeout_ms", "30000"}, {"room_hold_timeout_ms", "300000"}, {"address", "127.0.0.1"}, {"certificate_name", "localhost"},
        {"ca_file", ""}, {"certificate_sha256", ""}};
}
void put(Config& config, const std::string& key, std::string value) {
    const auto name = normalize(key);
    if (!config.contains(name)) throw std::runtime_error("Unknown configuration key");
    config[name] = std::move(value);
}
// Typed startup failure that carries its own EAWR-NET code.
struct ConfigError : std::runtime_error {
    unsigned code;
    ConfigError(unsigned number, const char* message) : std::runtime_error(message), code(number) {}
};
std::uint32_t number(const Config& config, const char* key) {
    const auto& text = config.at(key); std::uint32_t value{};
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) throw std::runtime_error("Invalid numeric configuration");
    return value;
}
eawr::net::RelayServerOptions options(const Config& c) {
    using namespace std::chrono;
    eawr::net::RelayServerOptions o;
    o.listen_address = c.at("listen_address");
    const auto port = number(c, "port"); if (port > 65535) throw std::runtime_error("Invalid port");
    o.port = static_cast<std::uint16_t>(port);
    o.certificate_file = c.at("certificate_file"); o.key_file = c.at("key_file"); o.access_token = c.at("access_token");
    const auto token_policy = number(c, "require_access_token"), alias_policy = number(c, "room_aliases");
    if (token_policy > 1 || alias_policy > 1) throw std::runtime_error("Invalid boolean configuration");
    o.require_access_token = token_policy != 0; o.room_aliases = alias_policy != 0;
    o.failed_guesses_per_minute = number(c, "failed_guesses_per_minute");
    o.turn_uri = c.at("turn_uri"); o.turn_secret = c.at("turn_secret");
    const auto& provider = c.at("turn_provider");
    if (provider == "coturn") o.turn_provider = eawr::net::TurnProvider::coturn;
    else if (provider == "cloudflare") o.turn_provider = eawr::net::TurnProvider::cloudflare;
    else throw ConfigError(1555, "Unknown turn_provider; use coturn or cloudflare");
    o.cloudflare_key_id = c.at("cloudflare_turn_key_id"); o.cloudflare_api_token = c.at("cloudflare_turn_api_token");
    o.cloudflare_calls_per_minute = number(c, "cloudflare_turn_calls_per_minute");
    o.cloudflare_timeout = milliseconds(number(c, "cloudflare_turn_timeout_ms"));
    // Sanitized warnings only: codes and fixed texts, never credentials or response bodies.
    o.diagnostics = [](const eawr::core::Diagnostic& d) { std::cerr << d.code << ": " << d.message << std::endl; };
    o.max_connections = number(c, "max_connections"); o.max_sessions = number(c, "max_sessions");
    o.connections_per_ip = number(c, "connections_per_ip"); o.accepts_per_minute = number(c, "accepts_per_minute");
    o.unpaired_sessions_per_ip = number(c, "unpaired_sessions_per_ip");
    o.operations_per_minute = number(c, "operations_per_minute"); o.bytes_per_minute = number(c, "bytes_per_minute");
    o.directory_operations_per_minute = number(c, "directory_operations_per_minute");
    o.directory_bytes_per_minute = number(c, "directory_bytes_per_minute");
    o.menu_operations_per_minute = number(c, "menu_operations_per_minute");
    o.menu_bytes_per_minute = number(c, "menu_bytes_per_minute"); o.menu_max_members = number(c, "menu_max_members");
    o.queue_bytes = number(c, "queue_bytes"); o.queue_frames = number(c, "queue_frames");
    o.admission_timeout = milliseconds(number(c, "admission_timeout_ms"));
    o.max_join_lifetime = milliseconds(number(c, "max_join_lifetime_ms")); o.idle_timeout = milliseconds(number(c, "idle_timeout_ms"));
    o.session_lifetime = milliseconds(number(c, "session_lifetime_ms"));
    o.max_session_lifetime = milliseconds(number(c, "max_session_lifetime_ms")); o.renewal_window = milliseconds(number(c, "renewal_window_ms"));
    o.room_operations_per_member_per_minute = number(c, "room_operations_per_member_per_minute");
    o.room_bytes_per_member_per_minute = number(c, "room_bytes_per_member_per_minute");
    o.room_reservation_timeout = milliseconds(number(c, "room_reservation_timeout_ms"));
    o.room_hold_timeout = milliseconds(number(c, "room_hold_timeout_ms")); return o;
}
int diagnostic(const eawr::core::Diagnostic& d) { std::cerr << d.code << ": " << d.message << '\n'; return 1; }
}
int main(int argc, char** argv) {
    try {
        Config config = defaults();
        std::string file;
        bool health{}, generate{}, fingerprint{}, token{}, health_name_explicit{};
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--certificate-name") health_name_explicit = true;
            if (arg == "--help") {
                std::cout << "eawr_relay_server [--config file] [--key value]\n"
                    "--health: TLS health probe; --address, --port, --certificate-name (system CA), optional --ca-file, or --certificate-sha256\n"
                    "--generate-certificate: create --certificate-file and --key-file (refuses overwrite)\n"
                    "--fingerprint: read --certificate-file PEM and print certificate DER SHA-256; --generate-token: print a random access token\n";
                return 0;
            }
            if (arg == "--health") { health = true; continue; }
            if (arg == "--generate-certificate") { generate = true; continue; }
            if (arg == "--fingerprint") { fingerprint = true; continue; }
            if (arg == "--generate-token") { token = true; continue; }
            if (!arg.starts_with("--") || i + 1 == argc) throw std::runtime_error("Expected --key value");
            if (arg == "--config") file = argv[i + 1];
            ++i;
        }
        if (!file.empty()) {
            std::ifstream input(file);
            if (!input) throw std::runtime_error("Cannot open config file");
            std::string line;
            while (std::getline(input, line)) {
                line = trim(std::move(line)); if (line.empty() || line.starts_with('#')) continue;
                const auto equal = line.find('='); if (equal == std::string::npos) throw std::runtime_error("Expected key=value in config");
                put(config, trim(line.substr(0, equal)), trim(line.substr(equal + 1)));
            }
        }
        for (auto& [key, value] : config) {
            auto env = "EAWR_RELAY_" + key;
            for (char& ch : env) if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
#ifdef _MSC_VER
            char* raw{}; std::size_t size{};
            if (_dupenv_s(&raw, &size, env.c_str()) != 0) throw std::runtime_error("Cannot read configuration environment");
            const std::unique_ptr<char, decltype(&std::free)> override_value(raw, std::free);
            if (override_value) value = override_value.get();
#else
            if (const char* override_value = std::getenv(env.c_str())) value = override_value;
#endif
        }
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--health" || arg == "--generate-certificate" || arg == "--fingerprint" || arg == "--generate-token") continue;
            if (arg != "--config") {
                // Secrets on argv show up in process listings and shell history.
                const auto name = normalize(arg.substr(2));
                if (name == "cloudflare_turn_key_id" || name == "cloudflare_turn_api_token")
                    throw ConfigError(1555, ("Set " + name + " in the environment or a protected config file, not on the command line").c_str());
                put(config, arg.substr(2), argv[i + 1]);
            }
            ++i;
        }
        using namespace eawr::net;
        if (token) {
            const auto a = relay::random_code(), b = relay::random_code();
            if (!a) return diagnostic(a.error());
            if (!b) return diagnostic(b.error());
            std::cout << a.value() << b.value() << '\n'; return 0;
        }
        if (generate) {
            if (config.at("certificate_file").empty() || config.at("key_file").empty()) throw std::runtime_error("Certificate and key paths are required");
            auto result = relay::create_certificate(config.at("certificate_file"), config.at("key_file"));
            if (!result) return diagnostic(result.error());
            fingerprint = true;
        }
        if (fingerprint) {
            auto result = relay::certificate_fingerprint(config.at("certificate_file"));
            if (!result) return diagnostic(result.error());
            std::cout << result.value() << '\n'; return 0;
        }
        if (health) {
            NetworkIntroductionOptions client;
            client.address = config.at("address"); const auto port = number(config, "port");
            if (port > 65535) throw std::runtime_error("Invalid port");
            client.port = static_cast<std::uint16_t>(port);
            client.certificate_name = config.at("certificate_name"); client.ca_file = config.at("ca_file");
            client.certificate_sha256 = config.at("certificate_sha256");
            if (!health_name_explicit && client.certificate_sha256.empty() && client.ca_file.empty() && !config.at("certificate_file").empty()) {
                auto pin = relay::certificate_fingerprint(config.at("certificate_file"));
                if (!pin) return diagnostic(pin.error());
                client.certificate_sha256 = std::move(pin).value();
            }
            auto result = NetworkGnsIntroduction::health(std::move(client));
            if (!result) return diagnostic(result.error());
            std::cout << "healthy\n"; return 0;
        }
        auto server = RelayServer::listen(options(config));
        if (!server) return diagnostic(server.error());
        std::signal(SIGINT, stop); std::signal(SIGTERM, stop);
        std::cout << "relay ready port=" << server.value()->port() << std::endl;
        while (!stopping) { server.value()->poll(); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        std::cout << "relay stopped\n";
        return 0;
    } catch (const ConfigError& e) {
        std::cerr << "EAWR-NET-" << e.code << ": " << e.what() << '\n';
        return 1;
    } catch (const std::exception&) {
        std::cerr << "EAWR-NET-1519: Invalid relay configuration or file operation\n";
        return 1;
    }
}
