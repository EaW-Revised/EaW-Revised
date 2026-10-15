#include "eawr/net/relay_server.hpp"
#include "relay_tls.hpp"
#include "relay_prefix_budget.hpp"
#include "relay_directory.hpp"
#include "relay_room.hpp"
#include "relay_hash_worker.hpp"
#include "relay_cloudflare.hpp"
#include "relay_renewal.hpp"
#include "relay_turn_refresh.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <functional>
#include <map>
#include <utility>

namespace eawr::net {
namespace {
using namespace relay;
bool provider_valid(const RelayServerOptions& o) {
    if (o.turn_provider == TurnProvider::coturn)
        return o.cloudflare_key_id.empty() && o.cloudflare_api_token.empty();
    return o.turn_uri.empty() && o.turn_secret.empty() && cloudflare_key_id_valid(o.cloudflare_key_id)
        && cloudflare_api_token_valid(o.cloudflare_api_token)
        && o.cloudflare_calls_per_minute > 0 && o.cloudflare_calls_per_minute <= 600
        && o.cloudflare_timeout >= std::chrono::milliseconds(500) && o.cloudflare_timeout <= std::chrono::seconds(30)
        && o.cloudflare_port != 0;
}
bool valid(const RelayServerOptions& o) {
    return !o.listen_address.empty() && !o.certificate_file.empty() && !o.key_file.empty()
        && (!o.require_access_token || (o.access_token.size() >= 32 && o.access_token.size() <= 256))
        && o.failed_guesses_per_minute > 0 && o.failed_guesses_per_minute <= 100000
        && turn_uri_valid(o.turn_uri) && (o.turn_uri.empty() == o.turn_secret.empty())
        && (o.turn_secret.empty() || (o.turn_secret.size() >= 32 && o.turn_secret.size() <= 256))
        && o.max_connections > 0 && o.max_connections <= 4096 && o.max_sessions > 0 && o.max_sessions <= 2048
        && o.connections_per_ip > 0 && o.connections_per_ip <= o.max_connections
        && o.unpaired_sessions_per_ip > 0 && o.unpaired_sessions_per_ip <= 2048
        && o.accepts_per_minute > 0 && o.accepts_per_minute <= 10000
        && o.operations_per_minute > 0 && o.operations_per_minute <= 100000
        && o.bytes_per_minute >= 1024 && o.bytes_per_minute <= 1024ULL * 1024 * 1024
        && o.directory_operations_per_minute > 0 && o.directory_operations_per_minute <= 100000
        && o.directory_bytes_per_minute >= 1024 && o.directory_bytes_per_minute <= 1024ULL * 1024 * 1024
        && o.queue_bytes >= max_body + 4 && o.queue_bytes <= 16 * 1024 * 1024
        && o.queue_frames > 0 && o.queue_frames <= 4096
        && o.admission_timeout.count() > 0 && o.admission_timeout <= std::chrono::seconds(60)
        && o.max_join_lifetime.count() > 0 && o.max_join_lifetime <= std::chrono::hours(24)
        && o.idle_timeout >= std::chrono::seconds(10) && o.idle_timeout <= std::chrono::hours(1)
        && o.session_lifetime >= std::chrono::seconds(1) && o.session_lifetime <= std::chrono::hours(24)
        && o.max_session_lifetime >= o.session_lifetime && o.max_session_lifetime <= std::chrono::hours(24 * 7)
        && o.renewal_window.count() > 0 && o.renewal_window <= o.max_session_lifetime
        && o.room_operations_per_member_per_minute > 0 && o.room_operations_per_member_per_minute <= 100000
        && o.room_bytes_per_member_per_minute >= max_body && o.room_bytes_per_member_per_minute <= 1024ULL * 1024 * 1024
        && o.room_reservation_timeout >= std::chrono::seconds(1) && o.room_reservation_timeout <= std::chrono::minutes(10)
        && o.room_hold_timeout >= std::chrono::seconds(1) && o.room_hold_timeout <= std::chrono::hours(24);
}
RoomOptions room_options(const RelayServerOptions& o) {
    RoomOptions r;
    r.session_lifetime = o.session_lifetime; r.max_session_lifetime = o.max_session_lifetime; r.renewal_window = o.renewal_window;
    r.reservation_timeout = o.room_reservation_timeout; r.hold_timeout = o.room_hold_timeout;
    // One provider call per member; a Cloudflare call may spend its whole timeout plus the connect margin.
    r.credential_wait = o.cloudflare_timeout + std::chrono::seconds(2);
    r.operations_per_member = o.room_operations_per_member_per_minute; r.bytes_per_member = o.room_bytes_per_member_per_minute;
    r.unpaired_per_prefix = o.unpaired_sessions_per_ip; r.failed_guesses = o.failed_guesses_per_minute; r.aliases = o.room_aliases;
    return r;
}
// Default credential refresh: the same HMAC (coturn REST) scheme as pairing. It completes inside submit.
class HmacTurnRefresh final : public TurnRefresh {
public:
    HmacTurnRefresh(std::string uri, std::string secret) : uri_(std::move(uri)), secret_(std::move(secret)) {}
    bool submit(TurnRefreshJob job) override {
        TurnRefreshResult result; result.session = std::move(job.session);
        auto issued = turn_credentials(uri_, secret_, job.lifetime);
        if (issued) result.credentials = std::move(issued).value(); else result.failure = issued.error();
        done_.push_back(std::move(result)); return true;
    }
    std::optional<TurnRefreshResult> take() override {
        if (done_.empty()) return {};
        auto result = std::move(done_.front()); done_.pop_front(); return result;
    }
private:
    std::string uri_, secret_;
    std::deque<TurnRefreshResult> done_;
};
// Cloudflare Realtime TURN refresh: one outbound HTTPS call per peer per renewal on a worker thread of its own, so the poll
// owner never blocks. The call cap is the same one pairing and revokes spend (turn_call_allowed).
class CloudflareTurnRefresh final : public TurnRefresh {
public:
    CloudflareTurnRefresh(CloudflareTurnConfig config, std::function<bool()> allowed)
        : worker_(std::move(config)), allowed_(std::move(allowed)) {}
    bool submit(TurnRefreshJob job) override {
        return allowed_() && worker_.submit({CloudflareTurnWorker::Kind::generate, std::move(job.session), job.lifetime, {}});
    }
    std::optional<TurnRefreshResult> take() override {
        auto done = worker_.take();
        if (!done) return {};
        TurnRefreshResult result; result.session = std::move(done->job.session);
        result.credentials = std::move(done->credentials); result.failure = std::move(done->failure);
        if (!result.credentials && !result.failure) result.failure = error(1558, "Cloudflare TURN returned no credentials");
        return result;
    }
private:
    CloudflareTurnWorker worker_;
    std::function<bool()> allowed_;
};
}
struct RelayServer::Impl {
    struct Connection {
        std::unique_ptr<TlsStream> tls;
        std::string ip, identity, session;
        Clock::time_point created{}, last_read{}, closing_at{};
        bool closing{};
        bool directory{}, hashing{}, room{};
        Bytes hash_request;
    };
    struct Session {
        std::array<std::uint64_t, 2> peers{};
        Clock::time_point expires{}, paired_at{};
        Budget budget;
        std::string alias, publisher_ownership;
        bool directory_bound{}, directory_admitted{}, invitation_revoked{};
        bool paired{};
        // Ticket E: the lease ends session_lifetime plus all renewals after pairing, never past the maximum.
        std::chrono::milliseconds extension{};
        bool warned{}, renewing{};
        std::uint64_t requester{};
        Clock::time_point renewal_deadline{};
        std::array<std::string, 2> refresh_keys{}, renewed_usernames{};
        std::array<std::optional<GnsTurnCredentials>, 2> refreshed{};
        // Cloudflare provider: the Paired message waits for the credential call (or its deadline).
        bool turn_pending{};
        Clock::time_point turn_deadline{};
        std::string turn_username;
    };
    RelayServerOptions options;
    Now now;
    socket::Open listener;
    Context context;
    std::uint64_t sequence{}, refresh_sequence{};
    std::map<std::string, std::pair<std::string, std::size_t>> refresh_jobs;
    std::map<std::uint64_t, Connection> clients;
    std::map<std::string, Session> sessions;
    PrefixBudgets ips;
    Directory directory;
    PasswordHashWorker hashes;
    std::unique_ptr<CloudflareTurnWorker> turns;
    std::deque<Clock::time_point> turn_calls;
    std::deque<std::string> revoke_queue;
    std::shared_ptr<TurnRefresh> refresh;
    // The room service's view of this server: queued output, typed failure, credentials and directory state.
    struct RoomHost final : RoomSink {
        Impl& server;
        explicit RoomHost(Impl& s) : server(s) {}
        bool send(std::uint64_t id, Bytes bytes) override {
            const auto found = server.clients.find(id); return found != server.clients.end() && server.output(found->second, std::move(bytes));
        }
        bool try_send(std::uint64_t id, Bytes bytes) override {
            // No fail(): a full queue of the receiver drops this blob and nothing else.
            const auto found = server.clients.find(id);
            return found != server.clients.end() && !found->second.closing && found->second.tls->queue(std::move(bytes));
        }
        void close(std::uint64_t id, Bytes frame) override {
            const auto found = server.clients.find(id);
            if (found == server.clients.end() || found->second.closing) return;
            auto& c = found->second;
            c.closing = true; c.closing_at = server.now() + std::chrono::milliseconds(250);
            if (!c.tls->ready() || !c.tls->queue(std::move(frame))) c.closing_at = server.now();
        }
        void fail(std::uint64_t id, unsigned reason) override {
            if (const auto found = server.clients.find(id); found != server.clients.end()) server.fail(found->second, reason);
        }
        bool request_turn(const std::string& key, std::chrono::seconds lifetime) override { return server.refresh->submit({key, lifetime}); }
        void revoke(const std::string& username) override { server.revoke_credentials(username); }
        std::optional<std::string> new_alias() override { return server.new_alias(); }
        void warn(core::Diagnostic diagnostic) override { server.warn(std::move(diagnostic)); }
        void directory_state(const std::string& room, bool locked, bool closed) override { server.directory.scope_state(room, locked, closed); }
    } room_host{*this};
    RoomService rooms;
    Impl(RelayServerOptions o, Now clock, socket::Open open, Context ctx, RelayServer::PasswordHash hash, std::shared_ptr<TurnRefresh> turn_refresh)
        : options(std::move(o)), now(clock), listener(open), context(std::move(ctx)), ips(options.max_connections),
          directory(options.max_sessions, options.max_connections), hashes(hash ? hash : password_hash),
          rooms(room_options(options), room_host, directory, ips) {
        directory.set_room_scope(&rooms);
        directory.set_bridge_hooks({
            [this](const std::string& code, const std::string& owner, bool passworded, bool publishing) { return bridge_valid(code, owner, passworded, publishing); },
            [this](const std::string& code) { sessions.at(code).directory_bound = true; },
            [this](const std::string& code) {
                const auto found = sessions.find(code);
                if (found == sessions.end() || !found->second.directory_bound || found->second.directory_admitted || found->second.paired || found->second.invitation_revoked) return false;
                found->second.directory_admitted = true; return true;
            },
            [this](const std::string& code) {
                const auto found = sessions.find(code);
                if (found == sessions.end() || found->second.paired || found->second.invitation_revoked) return;
                found->second.invitation_revoked = true;
                const auto host = clients.find(found->second.peers[0]); if (host != clients.end()) fail(host->second, 1523);
            }});
        if (options.turn_provider == TurnProvider::cloudflare) {
            CloudflareTurnConfig config;
            config.key_id = options.cloudflare_key_id; config.api_token = options.cloudflare_api_token;
            config.timeout = options.cloudflare_timeout; config.connect_address = options.cloudflare_connect_address;
            config.ca_file = options.cloudflare_ca_file; config.port = options.cloudflare_port;
            turns = std::make_unique<CloudflareTurnWorker>(config);
            // Renewals use their own worker thread so pairing completions keep a single consumer; both share the call cap.
            if (!turn_refresh) turn_refresh = std::make_shared<CloudflareTurnRefresh>(std::move(config), [this] { return turn_call_allowed(); });
        }
        if (!turn_refresh) turn_refresh = std::make_shared<HmacTurnRefresh>(options.turn_uri, options.turn_secret);
        refresh = std::move(turn_refresh);
    }
    ~Impl() { clients.clear(); socket::close(listener.handle); }
    void fail(Connection& c, unsigned reason) {
        if (c.closing) return;
        c.closing = true; c.closing_at = now() + std::chrono::milliseconds(250);
        // Before TLS readiness no authenticated error frame can be delivered.
        // Reclaim the handshake slot at its admission deadline immediately.
        if (!c.tls->ready()) { c.closing_at = now(); return; }
        auto bytes = message(Kind::error); u32(bytes, reason);
        if (!c.tls->queue(std::move(bytes))) c.closing_at = now();
    }
    bool output(Connection& c, Bytes bytes) {
        if (c.closing) return false;
        if (c.tls->queue(std::move(bytes))) return true;
        fail(c, 1526); return false;
    }
    void warn(core::Diagnostic diagnostic) {
        diagnostic.severity = core::Severity::warning;
        if (options.diagnostics) options.diagnostics(diagnostic);
    }
    // At most cloudflare_calls_per_minute outbound calls in any rolling minute, pairings and revokes together.
    bool turn_call_allowed() {
        const auto time = now();
        while (!turn_calls.empty() && time - turn_calls.front() >= std::chrono::minutes(1)) turn_calls.pop_front();
        if (turn_calls.size() >= options.cloudflare_calls_per_minute) return false;
        turn_calls.push_back(time); return true;
    }
    // A revoke that finds the shared call cap spent waits in a bounded queue and goes out as calls free up; one that
    // the provider refuses or fails leaves the credential to expire with its TTL.
    void revoke_credentials(const std::string& username) {
        if (!turns || username.empty()) return;
        if (!revoke_queue.empty() || !turn_call_allowed()) {
            if (revoke_queue.size() < 1024) revoke_queue.push_back(username);
            return;
        }
        if (!turns->submit({CloudflareTurnWorker::Kind::revoke, {}, {}, username}) && revoke_queue.size() < 1024) revoke_queue.push_back(username);
    }
    void drain_revokes() {
        while (turns && !revoke_queue.empty() && turn_call_allowed()) {
            if (!turns->submit({CloudflareTurnWorker::Kind::revoke, {}, {}, revoke_queue.front()})) break;
            revoke_queue.pop_front();
        }
    }
    void remove(std::uint64_t id) {
        const auto found = clients.find(id);
        if (found == clients.end()) return;
        directory.disconnect(id, now());
        rooms.disconnect(id, now());
        const auto session = sessions.find(found->second.session);
        if (session != sessions.end()) {
            directory.invalidate_bridge(session->first);
            if (!session->second.alias.empty()) directory.invalidate_bridge(session->second.alias);
            if (!session->second.paired) --ips.at(found->second.ip).unpaired;
            for (const auto other : session->second.peers)
                if (other != id && clients.contains(other)) fail(clients.at(other), 1528);
            abandon_refresh(session->second);
            revoke_credentials(session->second.turn_username);
            for (const auto& username : session->second.renewed_usernames) revoke_credentials(username);
            sessions.erase(session);
        }
        --ips.at(found->second.ip).live;
        clients.erase(found);
    }
    Bytes pairing(const std::string& code, const Connection& local, const Connection& remote,
            const GnsTurnCredentials& turn) {
        auto bytes = message(Kind::paired);
        string(bytes, code); string(bytes, local.identity); string(bytes, remote.identity);
        string(bytes, turn.uri); string(bytes, turn.username); string(bytes, turn.password);
        u32(bytes, static_cast<std::uint32_t>(turn.lifetime.count()));
        return bytes;
    }
    Clock::time_point lease_end(const Session& s) const { return s.paired_at + options.session_lifetime + s.extension; }
    Clock::time_point lease_hard_end(const Session& s) const { return s.paired_at + options.max_session_lifetime; }
    std::chrono::milliseconds renewal_window() const { return std::min(options.renewal_window, options.session_lifetime / 2); }
    static std::chrono::milliseconds until(Clock::time_point to, Clock::time_point from) {
        return to > from ? std::chrono::ceil<std::chrono::milliseconds>(to - from) : std::chrono::milliseconds{};
    }
    // Discard attempt keys before retrying: late completions must never extend a newer attempt.
    void abandon_refresh(Session& s) {
        for (std::size_t i = 0; i < s.refresh_keys.size(); ++i) {
            refresh_jobs.erase(s.refresh_keys[i]); s.refresh_keys[i].clear();
            if (s.refreshed[i]) revoke_credentials(s.refreshed[i]->username);
            s.refreshed[i].reset();
        }
        s.renewing = false;
    }
    void refuse_refresh(Session& s, Clock::time_point time) {
        abandon_refresh(s);
        if (const auto requester = clients.find(s.requester); requester != clients.end())
            output(requester->second, refused_message(renew_refused_credentials, until(lease_end(s), time)));
    }
    void renew(const std::string& code, Session& s, std::uint64_t id, Connection& c) {
        const auto time = now();
        const auto end = lease_end(s), hard = lease_hard_end(s);
        if (time >= end) { fail(c, end >= hard ? lease_ended_at_maximum : lease_not_renewed); return; }
        const auto remaining = until(end, time);
        if (end >= hard) { output(c, refused_message(renew_refused_maximum, remaining)); return; }
        if (s.renewing || s.turn_pending || remaining > renewal_window()) { output(c, refused_message(renew_refused_early, remaining)); return; }
        const auto next = std::min(end + options.session_lifetime, hard);
        const auto lifetime = std::min(std::chrono::ceil<std::chrono::seconds>(next - time) + std::chrono::seconds(60), std::chrono::seconds(172800));
        s.renewing = true; s.requester = id;
        // Two serial Cloudflare requests may each spend their configured timeout. Never wait beyond the lease.
        s.renewal_deadline = std::min(end, time + options.cloudflare_timeout * 2 + std::chrono::seconds(2));
        for (std::size_t i = 0; i < s.peers.size(); ++i) {
            auto key = code + ":" + std::to_string(++refresh_sequence);
            s.refresh_keys[i] = key;
            refresh_jobs.emplace(key, std::pair{code, i});
            if (!refresh->submit({std::move(key), lifetime})) { refuse_refresh(s, time); return; }
        }
    }
    void settle_renewals(Clock::time_point time) {
        // Check deadlines before completions, including completions delivered on the deadline poll.
        for (auto& [code, s] : sessions)
            if (s.renewing && time >= s.renewal_deadline) refuse_refresh(s, time);
        while (auto done = refresh->take()) {
            if (rooms.owns_turn(done->session)) {
                rooms.turn_done(done->session, std::move(done->credentials), std::move(done->failure), time); continue;
            }
            const auto job = refresh_jobs.find(done->session);
            if (job == refresh_jobs.end()) {
                if (done->credentials) revoke_credentials(done->credentials->username);
                continue;
            }
            const auto [code, peer] = job->second;
            refresh_jobs.erase(job);
            const auto found = sessions.find(code);
            if (found == sessions.end()) { if (done->credentials) revoke_credentials(done->credentials->username); continue; }
            auto& s = found->second;
            if (done->failure) warn(*done->failure);
            if (!done->credentials) { refuse_refresh(s, time); continue; }
            s.refreshed[peer] = std::move(done->credentials);
            if (!s.refreshed[0] || !s.refreshed[1]) continue;
            const auto end = lease_end(s), hard = lease_hard_end(s);
            s.extension += std::min(options.session_lifetime, std::chrono::duration_cast<std::chrono::milliseconds>(hard - end));
            s.warned = false; s.renewing = false;
            for (std::size_t i = 0; i < s.peers.size(); ++i) {
                // Keep pairing credentials alive: the adapter does not swap them on the existing allocation.
                // Older refresh credentials expire naturally; only the latest pair is retained for cleanup.
                if (turns) s.renewed_usernames[i] = s.refreshed[i]->username;
                const auto bytes = renewed_message(until(lease_end(s), time), until(hard, time), *s.refreshed[i]);
                if (const auto client = clients.find(s.peers[i]); client != clients.end()) output(client->second, bytes);
                s.refreshed[i].reset(); s.refresh_keys[i].clear();
            }
        }
        for (auto& [code, s] : sessions) {
            if (!s.paired || s.warned || s.renewing) continue;
            const auto end = lease_end(s), hard = lease_hard_end(s);
            if (time >= end || until(end, time) > renewal_window()) continue;
            s.warned = true;
            const auto bytes = expiring_message(until(end, time), until(hard, time), end < hard);
            for (const auto peer : s.peers) if (const auto client = clients.find(peer); client != clients.end()) output(client->second, bytes);
        }
    }
    void send_pairing(const std::string& code, const Session& s, const GnsTurnCredentials& turn) {
        const auto host = clients.find(s.peers[0]), guest = clients.find(s.peers[1]);
        if (host == clients.end() || guest == clients.end()) return;
        output(host->second, pairing(code, host->second, guest->second, turn));
        output(guest->second, pairing(code, guest->second, host->second, turn));
    }
    std::optional<std::string> new_alias() {
        constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            auto random = random_code(); if (!random) return {};
            std::string alias;
            const auto digit = [](char ch) { return ch <= '9' ? ch - '0' : ch - 'a' + 10; };
            for (std::size_t i = 0; i + 1 < random.value().size() && alias.size() < 4; i += 2) {
                const auto byte = digit(random.value()[i]) * 16 + digit(random.value()[i + 1]);
                if (byte < 252) alias += alphabet[static_cast<std::size_t>(byte) % alphabet.size()];
            }
            // One namespace for invitation aliases and room aliases: a typed code names exactly one thing.
            if (alias.size() == 4 && !rooms.alias_in_use(alias)
                && std::none_of(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.alias == alias; })) return alias;
        }
        return {};
    }
    bool guess_allowed(const Connection& c, bool alias_form) {
        return ips.guess_allowed(c.ip, now(), options.failed_guesses_per_minute, alias_form);
    }
    void failed_guess(const Connection& c, bool alias_form) {
        ips.failed_guess(c.ip, now(), alias_form);
    }
    bool bridge_valid(const std::string& invitation, const std::string& owner, bool passworded, bool publishing) const {
        if (!code_valid(invitation) || !code_valid(owner)) return false;
        const auto found = sessions.find(invitation);
        if (found == sessions.end() || found->second.paired || found->second.directory_admitted || found->second.invitation_revoked || now() >= found->second.expires) return false;
        if (!secret_equal(found->second.publisher_ownership, owner) || (publishing && found->second.directory_bound)) return false;
        const auto host = clients.find(found->second.peers[0]);
        if (host == clients.end() || host->second.closing) return false;
        return !passworded || found->second.alias.empty();
    }
    void request(std::uint64_t id, Connection& c, const Bytes& bytes) {
        if (bytes.size() < 2 || bytes[0] != 2) { fail(c, 1521); return; }
        const auto kind = static_cast<Kind>(bytes[1]);
        Reader reader{std::span(bytes).subspan(2)};
        if (room_kind(static_cast<unsigned>(kind))) {
            // Room scope: a member connection of its own, never a directory or legacy session connection.
            if (c.directory || !c.session.empty()) { fail(c, 1540); return; }
            c.room = true;
            rooms.request(id, c.ip, bytes, now()); return;
        }
        if (c.room) {
            // Ticket E's renewal and the client heartbeat run unchanged on a room member connection.
            if (kind == Kind::ping && reader.done()) return;
            if (kind == Kind::renew && reader.done()) { rooms.renew(id, now()); return; }
            fail(c, 1521); return;
        }
        if (static_cast<unsigned>(kind) >= 20 && static_cast<unsigned>(kind) <= 29) {
            if (!c.session.empty() || (!c.directory && kind != Kind::directory_open)) { fail(c, 1540); return; }
            if (kind == Kind::directory_publish) {
                Reader authorization{reader.bytes};
                const auto token = authorization.string(256);
                if (!authorization.valid || (options.require_access_token && !secret_equal(token, options.access_token))) {
                    auto reply = message(Kind::error); u32(reply, authorization.valid ? 1541 : 1540); output(c, std::move(reply)); return;
                }
            }
            std::optional<PasswordHashInput> pending;
            auto reply = directory.request(id, c.ip, bytes, now(), ips, &pending);
            if (pending) {
                if (hashes.submit({id, std::move(*pending)})) { c.hashing = true; c.hash_request = bytes; return; }
                reply = message(Kind::error); u32(reply, 1549);
            }
            if (kind == Kind::directory_open && reply[1] == static_cast<std::uint8_t>(Kind::directory_opened)) c.directory = true;
            if (!ips.at(c.ip).directory.add_bytes(reply.size(), options.directory_bytes_per_minute)) { fail(c, 1525); return; }
            output(c, std::move(reply)); return;
        }
        if (c.directory) { fail(c, 1540); return; }
        if (c.session.empty()) {
            if (kind == Kind::health && reader.done()) {
                output(c, message(Kind::healthy)); c.closing = true;
                c.closing_at = now() + std::chrono::milliseconds(250); return;
            }
            if (kind != Kind::publish && kind != Kind::join) { fail(c, 1521); return; }
            if (kind == Kind::publish) {
                const auto token = reader.string(256);
                if (!reader.valid || (options.require_access_token && !secret_equal(token, options.access_token))) { fail(c, 1520); return; }
            }
            std::string code;
            if (kind == Kind::join) code = reader.string(32);
            auto identity = reader.string(gns_identity_max_bytes);
            const auto lifetime = kind == Kind::publish ? reader.u32() : 0;
            const auto public_alias = kind == Kind::publish ? reader.u32() : 0;
            if (!reader.done() || !identity_valid(identity) || public_alias > 1
                || (kind == Kind::join && !code_valid(code) && !alias_valid(code))) { fail(c, 1521); return; }
            if (kind == Kind::publish) {
                if (lifetime == 0 || lifetime > 86400000) { fail(c, 1521); return; }
                if (sessions.size() >= options.max_sessions || ips.at(c.ip).unpaired >= options.unpaired_sessions_per_ip) {
                    fail(c, 1527); return;
                }
                auto generated = random_code(), owner = random_code();
                if (!generated || !owner || owner.value() == generated.value() || sessions.contains(generated.value())) { fail(c, 1529); return; }
                c.session = generated.value(); c.identity = std::move(identity);
                Session s; s.publisher_ownership = owner.value(); s.peers[0] = id; s.expires = now() + std::min(std::chrono::milliseconds(lifetime), options.max_join_lifetime); s.budget.window = now();
                if (options.room_aliases && public_alias != 0) {
                    auto alias = new_alias();
                    if (!alias) { c.session.clear(); fail(c, 1533); return; }
                    s.alias = std::move(*alias);
                }
                const auto alias = s.alias;
                const auto accepted = std::min(std::chrono::milliseconds(lifetime), options.max_join_lifetime);
                sessions.emplace(c.session, std::move(s));
                ++ips.at(c.ip).unpaired;
                auto response = message(Kind::published); string(response, c.session); string(response, c.identity); string(response, alias);
                u32(response, static_cast<std::uint32_t>(accepted.count())); string(response, owner.value());
                output(c, std::move(response)); return;
            }
            const bool alias_form = alias_valid(code);
            if (!guess_allowed(c, alias_form)) { fail(c, 1532); return; }
            if (alias_valid(code)) {
                const auto alias = std::find_if(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.alias == code; });
                if (alias == sessions.end()) { failed_guess(c, alias_form); fail(c, 1523); return; }
                code = alias->first;
            }
            const auto found = sessions.find(code);
            if (found == sessions.end()) { failed_guess(c, alias_form); fail(c, 1523); return; }
            auto& s = found->second;
            if (now() >= s.expires && !s.paired) { failed_guess(c, alias_form); fail(c, alias_form ? 1523 : 1522); return; }
            auto& host = clients.at(s.peers[0]);
            if (s.invitation_revoked) { failed_guess(c, alias_form); fail(c, 1523); return; }
            if (s.paired || host.closing || identity == host.identity) { failed_guess(c, alias_form); fail(c, 1524); return; }
            // The live allocation must cover all renewals; providers clamp credentials to 48 h.
            const auto credential_lifetime = std::min(std::chrono::ceil<std::chrono::seconds>(options.max_session_lifetime)
                + std::chrono::seconds(60), std::chrono::seconds(172800));
            if (options.turn_provider == TurnProvider::cloudflare) {
                c.session = code; c.identity = std::move(identity); s.peers[1] = id;
                --ips.at(host.ip).unpaired;
                s.paired = true; directory.close_bridge(code); s.paired_at = now();
                // The Paired message follows the credential call, which runs off this thread.
                const auto ttl = credential_lifetime;
                if (turn_call_allowed() && turns->submit({CloudflareTurnWorker::Kind::generate, code, ttl, {}})) {
                    s.turn_pending = true; s.turn_deadline = now() + options.cloudflare_timeout + std::chrono::seconds(2);
                    return;
                }
                warn(error(1559, "Cloudflare TURN call cap or queue reached; pairing continues without relay candidates"));
                send_pairing(code, s, GnsTurnCredentials{});
                return;
            }
            auto a = turn_credentials(options.turn_uri, options.turn_secret, credential_lifetime);
            auto b = turn_credentials(options.turn_uri, options.turn_secret, credential_lifetime);
            if (!a || !b) { fail(c, 1529); return; }
            c.session = code; c.identity = std::move(identity); s.peers[1] = id;
            --ips.at(host.ip).unpaired;
            s.paired = true; directory.close_bridge(code); s.paired_at = now();
            output(host, pairing(code, host, c, a.value()));
            output(c, pairing(code, c, host, b.value())); return;
        }
        const auto found = sessions.find(c.session);
        if (found == sessions.end()) { fail(c, 1528); return; }
        auto& s = found->second;
        s.budget.refresh(now());
        if (++s.budget.operations > options.operations_per_minute
            || !s.budget.add_bytes(bytes.size(), options.bytes_per_minute)) { fail(c, 1525); return; }
        if (kind == Kind::revoke && reader.done() && !s.paired && s.peers[0] == id) {
            s.invitation_revoked = true; // publisher gets its revoke acknowledgement, not a second failure
            directory.invalidate_bridge(found->first);
            if (!s.alias.empty()) directory.invalidate_bridge(s.alias);
            --ips.at(c.ip).unpaired; sessions.erase(found); c.session.clear();
            output(c, message(Kind::revoked)); c.closing = true;
            c.closing_at = now() + std::chrono::milliseconds(250); return;
        }
        if (kind == Kind::ping && reader.done()) return;
        if (kind == Kind::renew) {
            if (!reader.done()) { fail(c, 1521); return; }
            if (!s.paired) { fail(c, 1524); return; }
            renew(found->first, s, id, c); return;
        }
        if (kind != Kind::signal || reader.bytes.empty() || reader.bytes.size() > 65536) { fail(c, 1521); return; }
        if (!s.paired || s.turn_pending) { fail(c, 1524); return; }
        const auto peer = s.peers[0] == id ? s.peers[1] : s.peers[0];
        auto& target = clients.at(peer);
        auto forwarded = message(Kind::forwarded); string(forwarded, c.identity);
        forwarded.insert(forwarded.end(), reader.bytes.begin(), reader.bytes.end());
        if (!output(target, std::move(forwarded))) fail(c, 1526);
    }
    void poll() {
        const auto time = now();
        ips.refresh(time);
        directory.expire(time);
        settle_renewals(time);
        rooms.poll(time);
        drain_revokes();
        while (auto completed = hashes.take()) {
            const auto found = clients.find(completed->job.client);
            if (found == clients.end() || found->second.closing || !found->second.hashing) continue;
            auto& client = found->second; client.hashing = false;
            auto reply = directory.request(completed->job.client, client.ip, client.hash_request, time, ips, nullptr, &completed->output);
            client.hash_request.clear();
            if (!ips.at(client.ip).directory.add_bytes(reply.size(), options.directory_bytes_per_minute)) fail(client, 1525);
            else output(client, std::move(reply));
        }
        while (turns) {
            auto done = turns->take();
            if (!done) break;
            if (done->job.kind == CloudflareTurnWorker::Kind::revoke) {
                if (done->failure) warn(*done->failure);
                continue;
            }
            const auto found = sessions.find(done->job.session);
            if (done->failure) warn(*done->failure);
            if (found == sessions.end() || !found->second.turn_pending) {
                // The pairing ended or already continued without relay candidates.
                if (done->credentials) revoke_credentials(done->credentials->username);
                continue;
            }
            auto& s = found->second;
            s.turn_pending = false;
            if (done->credentials) s.turn_username = done->credentials->username;
            send_pairing(found->first, s, done->credentials ? *done->credentials : GnsTurnCredentials{});
        }
        for (auto& [code, s] : sessions) {
            if (!s.turn_pending || time < s.turn_deadline) continue;
            s.turn_pending = false;
            warn(error(1556, "Cloudflare TURN credentials were not ready in time; pairing continues without relay candidates"));
            send_pairing(code, s, GnsTurnCredentials{});
        }
        for (unsigned i = 0; i < 16; ++i) {
            const auto handle = socket::accept(listener.handle);
            if (handle == socket::invalid) break;
            const auto ip = address_prefix(socket::peer_address(handle));
            if (ip.empty() || clients.size() >= options.max_connections) { socket::close(handle); continue; }
            auto* admitted = ips.admit(ip, time, options.connections_per_ip, options.accepts_per_minute);
            if (!admitted) { socket::close(handle); continue; }
            auto& budget = *admitted;
            Connection c;
            c.ip = ip; c.created = c.last_read = time;
            c.tls = std::make_unique<TlsStream>(context, handle, true, "", "", options.queue_bytes, options.queue_frames);
            clients.emplace(++sequence, std::move(c));
            ++budget.live;
        }
        std::vector<std::uint64_t> dead;
        for (auto& [id, c] : clients) {
            c.tls->pump();
            if (c.closing) {
                // Drain bounded input while the typed final response and FIN
                // reach the sender. Closing a socket with unread pipelined
                // signals would issue a reset and discard that response.
                c.tls->receive();
                if (c.tls->ready() && c.tls->drained() && !c.tls->finish_writes()) dead.push_back(id);
            }
            auto& budget = ips.at(c.ip);
            auto& traffic = c.directory ? budget.directory : c.room ? budget.room : static_cast<Budget&>(budget);
            traffic.refresh(time);
            // A prefix may carry up to connections_per_ip member connections of the per-member room limits.
            const auto byte_limit = c.directory ? options.directory_bytes_per_minute
                : c.room ? options.room_bytes_per_member_per_minute * options.connections_per_ip : options.bytes_per_minute;
            if (!traffic.add_bytes(c.tls->take_read_bytes(), byte_limit)) fail(c, 1525);
            if (!c.closing && !c.hashing) if (auto bytes = c.tls->receive()) {
                c.last_read = time;
                const bool directory_request = bytes->size() >= 2 && (*bytes)[1] >= 20 && (*bytes)[1] <= 29;
                const bool room_request = c.room || (bytes->size() >= 2 && room_kind((*bytes)[1]));
                auto& operations = directory_request ? budget.directory : room_request ? budget.room : static_cast<Budget&>(budget);
                operations.refresh(time);
                const auto limit = directory_request ? options.directory_operations_per_minute
                    : room_request ? options.room_operations_per_member_per_minute * options.connections_per_ip : options.operations_per_minute;
                if (++operations.operations > limit) fail(c, 1525);
                else request(id, c, *bytes);
            }
            const auto session = sessions.find(c.session);
            const bool paired = session != sessions.end() && session->second.paired;
            if (!c.closing && !c.directory && !c.room && !paired && c.session.empty() && time - c.created >= options.admission_timeout) fail(c, 1528);
            if (!c.closing && session != sessions.end() && !paired && time >= session->second.expires) fail(c, 1522);
            if (!c.closing && (c.directory || c.room || !c.session.empty()) && time - c.last_read >= options.idle_timeout) fail(c, 1528);
            if (!c.closing && paired && time >= lease_end(session->second))
                fail(c, lease_end(session->second) >= lease_hard_end(session->second) ? lease_ended_at_maximum : lease_not_renewed);
            if (c.tls->failed() || (c.closing && time >= c.closing_at)) dead.push_back(id);
        }
        for (const auto id : dead) remove(id);
    }
};
RelayServer::RelayServer(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
RelayServer::~RelayServer() = default;
core::Result<std::unique_ptr<RelayServer>> RelayServer::listen(RelayServerOptions options, Now now, PasswordHash hash, std::shared_ptr<TurnRefresh> refresh) {
    if (now && !provider_valid(options)) return core::Result<std::unique_ptr<RelayServer>>::failure(relay::error(1555, "Invalid or mismatched TURN provider configuration"));
    if (!now || !valid(options)) return core::Result<std::unique_ptr<RelayServer>>::failure(relay::error(1519, "Invalid relay server options"));
    auto ctx = relay::server_context(options.certificate_file, options.key_file);
    if (!ctx) return core::Result<std::unique_ptr<RelayServer>>::failure(ctx.error());
    auto listener = socket::listen(options.listen_address, options.port);
    if (listener.handle == socket::invalid)
        return core::Result<std::unique_ptr<RelayServer>>::failure(relay::error(1519, "Relay listen failed"));
    return core::Result<std::unique_ptr<RelayServer>>::success(std::unique_ptr<RelayServer>(
        new RelayServer(std::make_unique<Impl>(std::move(options), now, listener, std::move(ctx).value(), hash, std::move(refresh)))));
}
void RelayServer::poll() { impl_->poll(); }
std::uint16_t RelayServer::port() const { return impl_->listener.port; }
std::size_t RelayServer::connections() const { return impl_->clients.size(); }
std::size_t RelayServer::sessions() const { return impl_->sessions.size(); }
std::size_t RelayServer::rooms() const { return impl_->rooms.rooms(); }
bool RelayServer::remove_chat_member(const std::string& room, std::uint32_t seat) {
    return impl_->directory.operator_remove(room, seat);
}
} // namespace eawr::net
