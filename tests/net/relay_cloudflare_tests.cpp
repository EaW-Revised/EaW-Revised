// Contracts for the Cloudflare Realtime TURN credential provider (#3627): response handling,
// the outbound HTTPS client against a local TLS endpoint, pairing through the relay server,
// bounded failure, numeric TURN resolution and secret hygiene. No test reaches the internet.
#include "eawr/net/network_introduction.hpp"
#include "eawr/net/relay_server.hpp"
#include "relay_cloudflare.hpp"
#include "relay_tls.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <optional>
#include <vector>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {
using namespace eawr::net;
using namespace eawr::net::relay;
using namespace std::chrono_literals;
void expect(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class T> T require(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + ": " + result.error().message);
    return std::move(result).value();
}
template<class F> void until(F predicate, const char* reason, std::chrono::milliseconds limit = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(reason);
        std::this_thread::sleep_for(2ms);
    }
}
constexpr const char* key_id = "keyid-Abc123_xyz";
constexpr const char* api_token = "tok_SECRET_9f8e7d6c5b4a39281706";
constexpr const char* service_name = "rtc.live.cloudflare.com";

std::string ice_servers(const std::string& urls, const std::string& username = "user0123456789", const std::string& credential = "cred+/abc=") {
    return "{\"iceServers\":[{\"urls\":[\"stun:stun.cloudflare.com:3478\"]},{\"urls\":[" + urls + "],\"username\":\"" + username
        + "\",\"credential\":\"" + credential + "\"}]}";
}
const std::string good_urls = "\"turn:203.0.113.7:3478?transport=udp\",\"turn:203.0.113.7:443?transport=udp\","
    "\"turn:203.0.113.7:3478?transport=tcp\",\"turns:203.0.113.7:5349?transport=tcp\"";

struct Reply { int status{201}; std::string body; std::chrono::milliseconds delay{}; bool chunked{}; };
struct Seen { std::string method, path, authorization, body; };

// Local TLS endpoint standing in for rtc.live.cloudflare.com. Its certificate names that host and is
// the only trust anchor the client under test is given, so name and chain checks stay on.
class FakeCloudflare {
public:
    FakeCloudflare(std::string certificate, std::string key) : certificate_(std::move(certificate)), key_(std::move(key)) {
        listener_ = socket::listen("127.0.0.1", 0);
        expect(listener_.handle != socket::invalid, "Fake endpoint listens");
        thread_ = std::jthread([this](std::stop_token stop) { serve(stop); });
    }
    ~FakeCloudflare() { thread_.request_stop(); thread_.join(); socket::close(listener_.handle); }
    std::uint16_t port() const { return listener_.port; }
    void push(Reply reply) { std::lock_guard lock(mutex_); replies_.push_back(std::move(reply)); }
    std::vector<Seen> seen() const { std::lock_guard lock(mutex_); return seen_; }
    std::size_t count(std::string_view suffix) const {
        std::lock_guard lock(mutex_);
        return static_cast<std::size_t>(std::count_if(seen_.begin(), seen_.end(), [&](const Seen& s) { return s.path.ends_with(suffix); }));
    }
private:
    std::string certificate_, key_;
    socket::Open listener_;
    mutable std::mutex mutex_;
    std::deque<Reply> replies_;
    std::vector<Seen> seen_;
    std::jthread thread_;
    template<class F> TlsStep drive(F step, std::stop_token stop) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        for (;;) {
            const auto result = step();
            if (result != TlsStep::wants_io) return result;
            if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline) return TlsStep::failed;
            std::this_thread::sleep_for(1ms);
        }
    }
    void serve(std::stop_token stop) {
        while (!stop.stop_requested()) {
            const auto handle = socket::accept(listener_.handle);
            if (handle == socket::invalid) { std::this_thread::sleep_for(2ms); continue; }
            exchange(handle, stop);
            socket::close(handle);
        }
    }
    void exchange(socket::Handle handle, std::stop_token stop) {
        TlsChannelOptions options;
        options.server = true; options.certificate = certificate_; options.key = key_;
        auto channel = open_tls_channel(handle, options);
        if (!channel || drive([&] { return channel->handshake(); }, stop) != TlsStep::done) return;
        std::string raw;
        std::size_t head = std::string::npos, length = 0;
        while (head == std::string::npos || raw.size() < head + 4 + length) {
            std::array<std::uint8_t, 2048> buffer{};
            std::size_t count{};
            if (drive([&] { return channel->read(buffer, count); }, stop) != TlsStep::done) return;
            raw.append(reinterpret_cast<const char*>(buffer.data()), count);
            if (head == std::string::npos && (head = raw.find("\r\n\r\n")) != std::string::npos) {
                const auto at = raw.find("Content-Length: ");
                if (at != std::string::npos && at < head) length = static_cast<std::size_t>(std::stoul(raw.substr(at + 16, 8)));
            }
        }
        Seen request;
        request.method = raw.substr(0, raw.find(' '));
        request.path = raw.substr(raw.find(' ') + 1, raw.find(' ', raw.find(' ') + 1) - raw.find(' ') - 1);
        if (const auto at = raw.find("Authorization: "); at != std::string::npos && at < head)
            request.authorization = raw.substr(at + 15, raw.find("\r\n", at) - at - 15);
        request.body = raw.substr(head + 4, length);
        Reply reply;
        {
            std::lock_guard lock(mutex_);
            seen_.push_back(request);
            if (request.path.ends_with("/revoke")) reply = Reply{204, {}, {}, false};
            else if (!replies_.empty()) { reply = std::move(replies_.front()); replies_.pop_front(); }
            else reply = Reply{500, "{}", {}, false};
        }
        for (auto waited = 0ms; waited < reply.delay && !stop.stop_requested(); waited += 10ms) std::this_thread::sleep_for(10ms);
        std::string body = reply.body;
        if (reply.chunked) {
            body.clear();
            for (std::size_t at = 0; at < reply.body.size(); at += 7) {
                const auto piece = reply.body.substr(at, 7);
                char size[16]; std::snprintf(size, sizeof(size), "%zx", piece.size());
                body += std::string(size) + "\r\n" + piece + "\r\n";
            }
            body += "0\r\n\r\n";
        }
        const auto text = "HTTP/1.1 " + std::to_string(reply.status) + " X\r\nContent-Type: application/json\r\n"
            + (reply.chunked ? std::string("Transfer-Encoding: chunked\r\n") : "Content-Length: " + std::to_string(body.size()) + "\r\n")
            + "Connection: close\r\n\r\n" + body;
        for (std::size_t offset = 0; offset < text.size();) {
            std::size_t written{};
            if (drive([&] { return channel->write(std::span(reinterpret_cast<const std::uint8_t*>(text.data()) + offset, text.size() - offset), written); }, stop) != TlsStep::done) return;
            offset += written;
        }
    }
};

CloudflareTurnConfig config_for(const FakeCloudflare& fake, const std::string& certificate) {
    CloudflareTurnConfig config;
    config.key_id = key_id; config.api_token = api_token; config.timeout = 2s;
    config.connect_address = "127.0.0.1"; config.port = fake.port(); config.ca_file = certificate;
    return config;
}
void no_secret(const std::string& text, const char* reason) {
    expect(text.find(api_token) == std::string::npos && text.find(key_id) == std::string::npos, reason);
}
void code_is(const eawr::core::Result<GnsTurnCredentials>& result, const char* code, const char* reason) {
    expect(!result && result.error().code == code, reason);
    no_secret(result.error().message, "Failure text never carries the key id or token");
}

void response_contracts() {
    const auto ttl = 7320s;
    auto good = require(cloudflare_credentials({201, ice_servers(good_urls)}, ttl));
    expect(good.uri == "turn:203.0.113.7:3478,turn:203.0.113.7:443", "Only UDP turn: URLs survive, without query, at most two");
    expect(good.username == "user0123456789" && good.password == "cred+/abc=" && good.lifetime == ttl, "Username, credential and TTL pass through");
    expect(turn_uri_list_valid(good.uri), "The wire list validates");
    const auto single = require(cloudflare_credentials({200, "{\"iceServers\":{\"urls\":\"turn:h.example:3478\",\"username\":\"u\",\"credential\":\"c\"}}"}, ttl));
    expect(single.uri == "turn:h.example:3478", "Single-object iceServers form is accepted");
    const auto escaped = require(cloudflare_credentials({201, ice_servers("\"turn:\\u0068.example:3478?transport=udp\"", "us\\/er", "c\\u0041")}, ttl));
    expect(escaped.uri == "turn:h.example:3478" && escaped.username == "us/er" && escaped.password == "cA", "JSON escapes decode");
    code_is(cloudflare_credentials({201, ice_servers("\"turn:h:3478\"", "user", "c\\u00e9")}, ttl), "EAWR-NET-1558", "Non-ASCII credentials are refused");

    code_is(cloudflare_credentials({403, "{\"error\":\"forbidden\"}"}, ttl), "EAWR-NET-1557", "4xx is a typed refusal");
    code_is(cloudflare_credentials({429, ""}, ttl), "EAWR-NET-1557", "429 is a typed refusal");
    code_is(cloudflare_credentials({503, ""}, ttl), "EAWR-NET-1557", "5xx is a typed refusal");
    for (const char* body : {"", "not json", "{\"iceServers\":", "[]", "{}", "{\"iceServers\":[]}", "{\"iceServers\":[1,2]}",
             "{\"iceServers\":[{\"urls\":[\"turn:h:3478\"],\"username\":\"u\"}]}", "{\"iceServers\":[{\"urls\":\"turn:h:3478\",\"username\":\"u\",\"credential\":\"c\"}]} trailing",
             "{\"iceServers\":[{\"urls\":\"turn:h:3478\",\"username\":\"\",\"credential\":\"c\"}]}",
             "{\"iceServers\":[{\"urls\":\"turn:h:3478\",\"username\":\"u,v\",\"credential\":\"c\"}]}",
             "{\"iceServers\":[{\"urls\":\"turn:h:3478\",\"username\":\"u\",\"credential\":\"bad\\u00\"}]}"})
        code_is(cloudflare_credentials({201, body}, ttl), "EAWR-NET-1558", "Malformed or incomplete JSON is typed");
    code_is(cloudflare_credentials({201, ice_servers("\"turns:h:5349?transport=tcp\",\"turn:h:3478?transport=tcp\",\"stun:h:3478\"")}, ttl),
        "EAWR-NET-1558", "A response without any UDP turn: URL is typed");
    const auto with_field = [](const std::string& value) {
        return "{\"note\":\"" + value + "\"," + ice_servers(good_urls).substr(1);
    };
    expect(cloudflare_credentials({201, with_field(std::string(3000, 'x'))}, ttl).has_value(), "A long string inside the bound parses");
    std::string escapes;
    for (int i = 0; i < 2100; ++i) escapes += "\\u0041";
    code_is(cloudflare_credentials({201, with_field(escapes)}, ttl), "EAWR-NET-1558", "Escape sequences count toward the string bound");
    std::string deep(40, '['); deep += std::string(40, ']');
    code_is(cloudflare_credentials({201, "{\"iceServers\":" + deep + "}"}, ttl), "EAWR-NET-1558", "Nesting is bounded");
}

void resolution_contracts() {
    const TurnHostResolver fake = [](const std::string& host, std::uint16_t) -> std::vector<std::string> {
        if (host == "turn.cloudflare.com") return {"198.51.100.1", "2001:db8::1", "198.51.100.2", "198.51.100.3"};
        if (host == "v6only.example") return {"2001:db8::7", "fe80::1%4"};
        return {};
    };
    expect(numeric_turn_uris("turn:turn.cloudflare.com:3478,turn:turn.cloudflare.com:443", fake)
            == "turn:198.51.100.1:3478,turn:198.51.100.2:3478,turn:198.51.100.1:443,turn:198.51.100.2:443",
        "Hostnames become up to two numeric IPv4 entries per URL");
    expect(numeric_turn_uris("turn:203.0.113.5:3478", fake) == "turn:203.0.113.5:3478", "Numeric coturn URIs are unchanged");
    expect(numeric_turn_uris("turn:203.0.113.5", fake) == "turn:203.0.113.5:3478", "A missing port gets the TURN default");
    expect(numeric_turn_uris("turn:[2001:db8::2]:3478", fake) == "turn:[2001:db8::2]:3478", "Bracketed IPv6 passes through");
    expect(numeric_turn_uris("turn:v6only.example:3478", fake) == "turn:[2001:db8::7]:3478", "IPv6 is used only without IPv4; scoped addresses are dropped");
    expect(numeric_turn_uris("turn:unknown.example:3478", fake).empty(), "An unresolved hostname yields no relay entry");
    expect(numeric_turn_uris("turn:bad host:3478,turn:turn.cloudflare.com:99999,turn:a/b:1", fake).empty(), "Malformed entries are dropped");
    expect(numeric_turn_uris("turn:turn.cloudflare.com:3478,turn:turn.cloudflare.com:3478", fake) == "turn:198.51.100.1:3478,turn:198.51.100.2:3478",
        "Duplicates collapse");
    expect(numeric_turn_uris("turn:999.1.1.1:3478,turn::3478,turn:[not-ip]:3478,turn:1.2.3.4:3478", fake) == "turn:1.2.3.4:3478",
        "Numeric hosts are validated; empty and malformed hosts are dropped");
    expect(numeric_turn_uris("turn:unknown.example:1,turn:unknown.example:2,turn:unknown.example:3,turn:unknown.example:4,turn:turn.cloudflare.com:3478", fake).empty(),
        "Only the first four entries are considered");
    std::stop_source stopped; stopped.request_stop();
    const auto started = std::chrono::steady_clock::now();
    expect(numeric_turn_uris("turn:localhost:3478", {}, 5s, stopped.get_token()).empty() && std::chrono::steady_clock::now() - started < 1s,
        "A stop request ends resolution at once");
}

void provider_contracts(const std::string& fake_certificate, const std::string& fake_key, const std::string& wrong_name_certificate,
        const std::string& wrong_name_key) {
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        const auto result = require(cloudflare_generate(config_for(fake, fake_certificate), 7320s, {}));
        expect(result.uri == "turn:203.0.113.7:3478,turn:203.0.113.7:443" && result.username == "user0123456789", "Success over verified TLS");
        const auto seen = fake.seen();
        expect(seen.size() == 1 && seen[0].method == "POST", "One POST per credential request");
        expect(seen[0].path == std::string("/v1/turn/keys/") + key_id + "/credentials/generate-ice-servers", "Documented generate-ice-servers path");
        expect(seen[0].authorization == std::string("Bearer ") + api_token, "Bearer token travels in the Authorization header");
        expect(seen[0].body == "{\"ttl\":7320}", "TTL body");
        expect(seen[0].path.find(api_token) == std::string::npos && seen[0].body.find(api_token) == std::string::npos, "Token never appears in path or body");
    }
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        Reply chunked{201, ice_servers(good_urls), {}, true};
        fake.push(chunked);
        expect(require(cloudflare_generate(config_for(fake, fake_certificate), 600s, {})).username == "user0123456789", "Chunked responses decode");
        fake.push({403, "{\"error\":\"nope\"}"});
        code_is(cloudflare_generate(config_for(fake, fake_certificate), 600s, {}), "EAWR-NET-1557", "403 from the endpoint");
        fake.push({201, "{broken"});
        code_is(cloudflare_generate(config_for(fake, fake_certificate), 600s, {}), "EAWR-NET-1558", "Malformed JSON from the endpoint");
        fake.push({201, ice_servers("\"turns:h:5349?transport=tcp\"")});
        code_is(cloudflare_generate(config_for(fake, fake_certificate), 600s, {}), "EAWR-NET-1558", "Missing UDP URL from the endpoint");
    }
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls), 3s});
        auto config = config_for(fake, fake_certificate); config.timeout = 400ms;
        const auto started = std::chrono::steady_clock::now();
        code_is(cloudflare_generate(config, 600s, {}), "EAWR-NET-1556", "A slow endpoint times out with a typed failure");
        expect(std::chrono::steady_clock::now() - started < 2s, "The request timeout bounds the wait");
    }
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls), 3s});
        std::stop_source source; auto config = config_for(fake, fake_certificate); config.timeout = 10s;
        std::jthread stopper([&] { std::this_thread::sleep_for(200ms); source.request_stop(); });
        const auto started = std::chrono::steady_clock::now();
        code_is(cloudflare_generate(config, 600s, source.get_token()), "EAWR-NET-1556", "Shutdown interrupts a request");
        expect(std::chrono::steady_clock::now() - started < 2s, "Stop is honoured promptly");
    }
    {
        // Name check: a trusted certificate for the wrong host name is refused.
        FakeCloudflare fake(wrong_name_certificate, wrong_name_key);
        fake.push({201, ice_servers(good_urls)});
        code_is(cloudflare_generate(config_for(fake, wrong_name_certificate), 600s, {}), "EAWR-NET-1556", "Certificate for another host name is refused");
        // Chain check: the right name signed by an anchor the client does not trust is refused.
        FakeCloudflare other(fake_certificate, fake_key);
        other.push({201, ice_servers(good_urls)});
        code_is(cloudflare_generate(config_for(other, wrong_name_certificate), 600s, {}), "EAWR-NET-1556", "Untrusted certificate is refused");
        expect(fake.seen().empty() && other.seen().empty(), "No request is sent after a failed verification");
    }
    {
        // The operating system trust store is the default and refuses an endpoint it has no anchor for.
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        auto config = config_for(fake, fake_certificate); config.ca_file.clear();
#ifndef _WIN32
        unsetenv("SSL_CERT_FILE");
#endif
        code_is(cloudflare_generate(config, 600s, {}), "EAWR-NET-1556", "The default trust store refuses an unknown anchor");
        expect(fake.seen().empty(), "Nothing is sent over an unverified connection");
#ifndef _WIN32
        // OpenSSL's default verify paths honour SSL_CERT_FILE, which makes the system-store path testable.
        expect(setenv("SSL_CERT_FILE", fake_certificate.c_str(), 1) == 0, "Set SSL_CERT_FILE");
        const auto trusted = cloudflare_generate(config, 600s, {});
        unsetenv("SSL_CERT_FILE");
        expect(trusted && trusted.value().username == "user0123456789", "The default trust store verifies a certificate it trusts");
#endif
    }
    {
        CloudflareTurnConfig down;
        down.key_id = key_id; down.api_token = api_token; down.timeout = 400ms;
        down.connect_address = "127.0.0.1"; down.ca_file = fake_certificate;
        const auto closed = socket::listen("127.0.0.1", 0); socket::close(closed.handle);
        down.port = closed.port;
        code_is(cloudflare_generate(down, 600s, {}), "EAWR-NET-1556", "A refused connection is a typed failure");
    }
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        require(cloudflare_revoke(config_for(fake, fake_certificate), "user:a b/c", {}));
        const auto seen = fake.seen();
        expect(seen.size() == 1 && seen[0].method == "POST"
            && seen[0].path == std::string("/v1/turn/keys/") + key_id + "/credentials/user%3Aa%20b%2Fc/revoke", "Revoke posts to the documented path with an encoded username");
        auto config = config_for(fake, fake_certificate); config.key_id = "bad key";
        expect(!cloudflare_key_id_valid(config.key_id) && !cloudflare_api_token_valid("white space") && cloudflare_api_token_valid(api_token),
            "Key id and token shapes are validated");
    }
}

struct Diagnostics {
    std::mutex mutex;
    std::vector<std::string> lines;
    void add(const eawr::core::Diagnostic& d) { std::lock_guard lock(mutex); lines.push_back(d.code + " " + d.message); }
    bool has(const std::string& code) {
        std::lock_guard lock(mutex);
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& l) { return l.starts_with(code); });
    }
    std::string all() { std::lock_guard lock(mutex); std::string text; for (const auto& l : lines) text += l + "\n"; return text; }
};
struct Fixture {
    std::unique_ptr<RelayServer> server;
    NetworkIntroductionOptions client;
    Diagnostics diagnostics;
    std::jthread worker;
    Fixture(const FakeCloudflare& fake, const std::string& fake_certificate, const std::string& certificate, const std::string& key,
            std::size_t calls_per_minute = 30, std::chrono::milliseconds timeout = 2s,
            std::chrono::milliseconds session_lifetime = 7200000ms, std::chrono::milliseconds max_session_lifetime = 0ms,
            std::chrono::milliseconds renewal_window = 0ms) {
        RelayServerOptions options;
        options.port = 0; options.listen_address = "127.0.0.1"; options.certificate_file = certificate; options.key_file = key;
        options.turn_provider = TurnProvider::cloudflare; options.cloudflare_key_id = key_id; options.cloudflare_api_token = api_token;
        options.cloudflare_calls_per_minute = calls_per_minute; options.cloudflare_timeout = timeout; options.session_lifetime = session_lifetime;
        if (max_session_lifetime.count() > 0) options.max_session_lifetime = max_session_lifetime;
        if (renewal_window.count() > 0) options.renewal_window = renewal_window;
        options.cloudflare_connect_address = "127.0.0.1"; options.cloudflare_port = fake.port(); options.cloudflare_ca_file = fake_certificate;
        options.diagnostics = [this](const eawr::core::Diagnostic& d) { diagnostics.add(d); };
        server = require(RelayServer::listen(std::move(options)));
        client.address = "127.0.0.1"; client.port = server->port(); client.certificate_name = "localhost";
        client.ca_file = certificate; client.timeout = 6s;
        worker = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) { server->poll(); std::this_thread::sleep_for(1ms); }
        });
    }
    ~Fixture() { worker.request_stop(); worker.join(); }
};
struct Pair { std::shared_ptr<GnsSignallingEndpoint> host, guest; };
Pair pair_up(Fixture& f, const std::string& host = "host-peer", const std::string& guest = "client-peer") {
    NetworkGnsIntroduction service(f.client);
    auto published = require(service.publish(host, 20s));
    Pair pair;
    pair.host = published.endpoint;
    pair.guest = require(service.join(published.join_code, guest));
    until([&] { return pair.host->rendezvous().remote_identity == guest; }, "Host receives the paired identity");
    return pair;
}

void pairing_contracts(const std::string& fake_certificate, const std::string& fake_key, const std::string& certificate, const std::string& key) {
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        Fixture f(fake, fake_certificate, certificate, key);
        auto pair = pair_up(f);
        const auto host = pair.host->turn_credentials(), guest = pair.guest->turn_credentials();
        expect(host && guest, "Both peers receive relay credentials");
        expect(host->uri == "turn:203.0.113.7:3478,turn:203.0.113.7:443" && host->username == "user0123456789" && host->password == "cred+/abc=", "Provider credentials reach the host");
        expect(host->uri == guest->uri && host->username == guest->username && host->password == guest->password && host->lifetime == guest->lifetime,
            "One provider call serves both peers with the same wire fields");
        expect(host->lifetime == 86460s, "Pairing TTL covers the maximum session plus a margin");
        const auto seen = fake.seen();
        expect(seen.size() == 1 && seen[0].body == "{\"ttl\":86460}" && seen[0].authorization == std::string("Bearer ") + api_token,
            "Exactly one outbound call per pairing");
        const Bytes payload{1, 2, 3};
        require(pair.host->send(payload));
        std::optional<GnsSignal> signal;
        until([&] { signal = require(pair.guest->poll()); return signal.has_value(); }, "Signals flow after a delayed pairing");
        expect(signal->bytes == payload, "Signal contents survive");
        // Session end: the credentials are revoked once.
        pair.host.reset(); pair.guest.reset();
        until([&] { return fake.count("/revoke") == 1; }, "Credentials are revoked at session end");
        expect(fake.seen().back().path == std::string("/v1/turn/keys/") + key_id + "/credentials/user0123456789/revoke", "Revoke targets the issued username");
        no_secret(f.diagnostics.all(), "Diagnostics never carry the key id or token");
    }
    {
        // The longest allowed session stays far inside Cloudflare's 48 h ttl ceiling.
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        Fixture f(fake, fake_certificate, certificate, key, 30, 2s, 86400000ms);
        auto pair = pair_up(f);
        const auto seen = fake.seen();
        expect(seen.size() == 1 && seen[0].body == "{\"ttl\":86460}", "ttl follows the maximum session, not the lease");
        expect(pair.host->turn_credentials()->lifetime <= 172800s, "ttl stays under 48 hours");
    }
    {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        Fixture f(fake, fake_certificate, certificate, key, 30, 2s, 7200000ms, 604800000ms);
        auto pair = pair_up(f);
        expect(pair.host->turn_credentials()->lifetime == 172800s, "Pairing TTL never exceeds the 48 h provider clamp");
        expect(fake.seen()[0].body == "{\"ttl\":172800}", "Provider receives the clamped TTL");
    }
    {
        // A player leaves while the credential call is in flight: no crash, and the late credentials are revoked once.
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls), 800ms});
        Fixture f(fake, fake_certificate, certificate, key, 30, 4s);
        NetworkGnsIntroduction service(f.client);
        auto published = require(service.publish("host-peer", 20s));
        std::jthread guest([&] { NetworkGnsIntroduction other(f.client); static_cast<void>(other.join(published.join_code, "client-peer")); });
        until([&] { return fake.count("/generate-ice-servers") == 1; }, "The credential call starts");
        published.endpoint.reset();
        until([&] { return fake.count("/revoke") == 1; }, "Late credentials are revoked");
        std::this_thread::sleep_for(300ms);
        expect(fake.count("/revoke") == 1 && fake.count("/generate-ice-servers") == 1, "Exactly one revoke and no retry");
        guest.join();
        no_secret(f.diagnostics.all(), "Disconnect path logs no secrets");
    }
    {
        // Hostnames are resolved by the client before GNS sees them.
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers("\"turn:localhost:3478?transport=udp\"")});
        Fixture f(fake, fake_certificate, certificate, key);
        auto pair = pair_up(f);
        const auto guest = pair.guest->turn_credentials();
        expect(guest && guest->uri.find("localhost") == std::string::npos && guest->uri.starts_with("turn:"), "Client rewrites TURN hostnames to numeric addresses");
    }
    const std::vector<std::pair<const char*, Reply>> failures{
        {"EAWR-NET-1557", Reply{403, "{}", {}, false}}, {"EAWR-NET-1558", Reply{201, "{broken", {}, false}},
        {"EAWR-NET-1558", Reply{201, ice_servers("\"turns:h:5349?transport=tcp\""), {}, false}},
        {"EAWR-NET-1556", Reply{201, ice_servers(good_urls), 3s, false}}};
    for (const auto& scenario : failures) {
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push(scenario.second);
        Fixture f(fake, fake_certificate, certificate, key, 30, 500ms);
        auto pair = pair_up(f);
        expect(!pair.host->turn_credentials() && !pair.guest->turn_credentials(), "A failed provider call pairs without relay candidates");
        expect(pair.guest->rendezvous().remote_identity == "host-peer" && pair.host->rendezvous().remote_identity == "client-peer", "The rendezvous still completes");
        until([&] { return f.diagnostics.has(scenario.first); }, "A typed warning is logged");
        no_secret(f.diagnostics.all(), "Warnings never carry the key id or token");
        expect(fake.count("/generate-ice-servers") == 1, "No retry storm after a failure");
        std::this_thread::sleep_for(300ms);
        expect(fake.count("/generate-ice-servers") == 1, "Still a single call");
    }
    {
        // Per-minute cap: the second pairing proceeds without relay candidates and without a call.
        FakeCloudflare fake(fake_certificate, fake_key);
        fake.push({201, ice_servers(good_urls)});
        Fixture f(fake, fake_certificate, certificate, key, 1);
        auto first = pair_up(f, "host-a", "guest-a");
        expect(first.host->turn_credentials().has_value(), "The first pairing is served");
        auto second = pair_up(f, "host-b", "guest-b");
        expect(!second.host->turn_credentials() && !second.guest->turn_credentials(), "Over the cap pairs without relay candidates");
        until([&] { return f.diagnostics.has("EAWR-NET-1559"); }, "The cap is reported with a typed warning");
        expect(fake.count("/generate-ice-servers") == 1, "The cap bounds outbound calls");
        no_secret(f.diagnostics.all(), "Cap warnings carry no secrets");
    }
}

// Ticket E: renewal requests one credential per peer; the pairing credential remains usable.
void renewal_contracts(const std::string& fake_certificate, const std::string& fake_key, const std::string& certificate, const std::string& key) {
    FakeCloudflare fake(fake_certificate, fake_key);
    fake.push({201, ice_servers(good_urls)});
    fake.push({201, ice_servers(good_urls, "renewed-user-01", "renewed-cred=")});
    fake.push({201, ice_servers(good_urls, "renewed-user-02", "guest-cred=")});
    // A 4 s lease warns at its last 2 s; both peers then ask, and the service makes two provider calls.
    Fixture f(fake, fake_certificate, certificate, key, 30, 2s, 4000ms, 10000ms, 3000ms);
    auto pair = pair_up(f);
    std::optional<GnsTurnCredentials> host, guest;
    until([&] { if (!host) host = pair.host->take_refreshed_turn_credentials(); if (!guest) guest = pair.guest->take_refreshed_turn_credentials(); return host && guest; },
        "Both peers receive the renewed credentials", 8s);
    expect(host->username == "renewed-user-01" && host->password == "renewed-cred=" && host->uri == "turn:203.0.113.7:3478,turn:203.0.113.7:443",
        "Renewed credentials come from the provider's second call");
    expect(guest->username == "renewed-user-02" && guest->password == "guest-cred=" && host->lifetime == guest->lifetime && host->lifetime <= 70s && host->lifetime > 60s, "Each peer gets its own credential, sized to the new lease");
    expect(fake.count("/generate-ice-servers") == 3, "Two provider calls per renewal, one per peer");
    expect(fake.count("/revoke") == 0, "Renewal retains the pairing credential for the live allocation");
    const auto lease = pair.host->lease();
    expect(lease && lease->renewals == 1, "The endpoint counts the renewal");
    no_secret(f.diagnostics.all(), "Renewal diagnostics carry no secrets");
    pair.host.reset(); pair.guest.reset();
    until([&] { return fake.count("/revoke") == 3; }, "Pairing and both renewed credentials are revoked at session end");
}

void startup_contracts(const std::string& certificate, const std::string& key) {
    const auto code = [&](RelayServerOptions options) -> std::string {
        options.port = 0; options.listen_address = "127.0.0.1"; options.certificate_file = certificate; options.key_file = key;
        auto result = RelayServer::listen(std::move(options));
        return result ? std::string{} : result.error().code;
    };
    const auto cloudflare = [] {
        RelayServerOptions options;
        options.turn_provider = TurnProvider::cloudflare; options.cloudflare_key_id = key_id; options.cloudflare_api_token = api_token;
        return options;
    };
    expect(code(cloudflare()).empty(), "A complete cloudflare configuration starts");
    expect(code(RelayServerOptions{}).empty(), "The coturn default starts without a TURN route");
    { auto o = cloudflare(); o.cloudflare_key_id.clear(); expect(code(o) == "EAWR-NET-1555", "Missing key id fails startup typed"); }
    { auto o = cloudflare(); o.cloudflare_api_token.clear(); expect(code(o) == "EAWR-NET-1555", "Missing token fails startup typed"); }
    { auto o = cloudflare(); o.cloudflare_key_id = "bad/key"; expect(code(o) == "EAWR-NET-1555", "Key id with path characters fails"); }
    { auto o = cloudflare(); o.cloudflare_api_token = "has space"; expect(code(o) == "EAWR-NET-1555", "Token with whitespace fails"); }
    { auto o = cloudflare(); o.turn_uri = "turn:127.0.0.1:3478"; o.turn_secret = std::string(64, 'b'); expect(code(o) == "EAWR-NET-1555", "cloudflare plus coturn settings is a mismatch"); }
    { RelayServerOptions o; o.cloudflare_api_token = api_token; expect(code(o) == "EAWR-NET-1555", "coturn plus Cloudflare secrets is a mismatch"); }
    { auto o = cloudflare(); o.cloudflare_calls_per_minute = 0; expect(code(o) == "EAWR-NET-1555", "A zero call cap fails"); }
    { auto o = cloudflare(); o.cloudflare_timeout = 0ms; expect(code(o) == "EAWR-NET-1555", "A zero timeout fails"); }
    { RelayServerOptions o; o.turn_uri = "turn:127.0.0.1:3478"; expect(code(o) == "EAWR-NET-1519", "coturn URI/secret mismatch keeps its existing error"); }
}
} // namespace

int main(int argc, char** argv) {
    try {
        expect(argc == 2, "Artifact directory argument");
        const auto directory = std::filesystem::path(argv[1]) / require(random_code());
        std::filesystem::create_directories(directory);
        const auto path = [&](const char* name) { return (directory / name).string(); };
        require(create_certificate(path("relay.pem"), path("relay.key")));
        require(create_certificate(path("cf.pem"), path("cf.key"), std::chrono::hours(1), service_name));
        require(create_certificate(path("other.pem"), path("other.key"), std::chrono::hours(1), "not-cloudflare.example"));
        response_contracts();
        resolution_contracts();
        provider_contracts(path("cf.pem"), path("cf.key"), path("other.pem"), path("other.key"));
        pairing_contracts(path("cf.pem"), path("cf.key"), path("relay.pem"), path("relay.key"));
        renewal_contracts(path("cf.pem"), path("cf.key"), path("relay.pem"), path("relay.key"));
        startup_contracts(path("relay.pem"), path("relay.key"));
        std::filesystem::remove_all(directory);
        std::cout << "Cloudflare TURN provider contracts passed\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
