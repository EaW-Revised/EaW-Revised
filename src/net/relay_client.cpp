#include "eawr/net/network_introduction.hpp"
#include "relay_protocol.hpp"
#include "relay_renewal.hpp"
#include "relay_directory_codec.hpp"
#include "relay_menu.hpp"
#include "relay_client_options.hpp"

#if EAWR_HAS_RELAY
#include "relay_tls.hpp"
#include "relay_https.hpp"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <stop_token>
#endif

namespace eawr::net {
using namespace relay;
#if EAWR_HAS_RELAY
NetworkGnsIntroduction::NetworkGnsIntroduction(NetworkIntroductionOptions options) : options_(std::move(options)) {}
bool NetworkGnsIntroduction::available() { return true; }
namespace {
using Clock = std::chrono::steady_clock;
bool valid(const NetworkIntroductionOptions& o, bool token_optional) { return client_options_valid(o, token_optional); }
// GNS takes numeric TURN addresses only. A Paired message that lists hostnames (Cloudflare) is rewritten
// here on the endpoint worker thread, never the game thread; numeric lists (coturn) pass through byte for byte.
// When no hostname resolves the message keeps its rendezvous and carries no relay credentials.
Bytes numeric_turn(Bytes bytes, std::stop_token stop) {
    if (bytes.size() < 2 || bytes[0] != 2 || static_cast<Kind>(bytes[1]) != Kind::paired) return bytes;
    Reader reader{std::span(bytes).subspan(2)};
    const auto session = reader.string(32), local = reader.string(gns_identity_max_bytes), remote = reader.string(gns_identity_max_bytes);
    const auto uri = reader.string(512), username = reader.string(256), password = reader.string(256);
    const auto lifetime = reader.u32();
    if (!reader.done() || uri.empty() || !turn_uri_list_valid(uri)) return bytes;
    const auto numeric = numeric_turn_uris(uri, {}, std::chrono::seconds(5), stop);
    if (numeric == uri) return bytes;
    auto result = message(Kind::paired);
    string(result, session); string(result, local); string(result, remote);
    string(result, numeric); string(result, numeric.empty() ? "" : username); string(result, numeric.empty() ? "" : password);
    u32(result, numeric.empty() ? 0 : lifetime);
    return result;
}
class NetworkEndpoint final : public GnsSignallingEndpoint {
public:
    explicit NetworkEndpoint(NetworkIntroductionOptions options, Bytes request)
        : options_(std::move(options)), request_(std::move(request)) {
        Reader reader{std::span(request_).subspan(2)};
        if (request_[1] != static_cast<std::uint8_t>(Kind::health)) {
            if (request_[1] == static_cast<std::uint8_t>(Kind::publish)) reader.string(256);
            if (request_[1] == static_cast<std::uint8_t>(Kind::join)) expected_code_ = reader.string(32);
            expected_identity_ = reader.string(gns_identity_max_bytes);
        }
    }
    ~NetworkEndpoint() override {
        worker_.request_stop();
        if (worker_.joinable()) worker_.join();
    }
    void start() { worker_ = std::jthread([this](std::stop_token stop) { run(stop); }); }
    core::Result<void> wait() {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, options_.timeout, [&] { return admitted_ || failure_; }))
            failure_ = error(1528, "Relay introduction request timed out");
        if (failure_) return core::Result<void>::failure(*failure_);
        return core::Result<void>::success();
    }
    std::string ownership() const { std::lock_guard lock(mutex_); return publisher_ownership_; }
    std::string alias() const { std::lock_guard lock(mutex_); return alias_; }
    Clock::time_point expires() const { std::lock_guard lock(mutex_); return expires_; }
    GnsRendezvous rendezvous() const override { std::lock_guard lock(mutex_); return rendezvous_; }
    std::optional<GnsTurnCredentials> turn_credentials() const override { std::lock_guard lock(mutex_); return turn_; }
    std::optional<GnsSessionLease> lease() const override { std::lock_guard lock(mutex_); return lease_; }
    std::optional<GnsTurnCredentials> take_refreshed_turn_credentials() override {
        std::lock_guard lock(mutex_); auto result = std::move(refreshed_); refreshed_.reset(); return result;
    }
    core::Result<void> status() const override {
        const std::lock_guard lock(mutex_);
        return failure_ ? core::Result<void>::failure(*failure_) : core::Result<void>::success();
    }
    core::Result<void> close_publication() override {
        std::unique_lock lock(mutex_);
        if (request_[1] != static_cast<std::uint8_t>(Kind::publish) || !rendezvous_.remote_identity.empty())
            return core::Result<void>::failure(error(1534, "Only an unused publication can be closed"));
        close_requested_ = true;
        if (!condition_.wait_for(lock, options_.timeout, [&] { return publication_closed_ || failure_; }) || !publication_closed_)
            return core::Result<void>::failure(error(1534, "Publication close was not confirmed"));
        return core::Result<void>::success();
    }
    core::Result<void> send(std::span<const std::uint8_t> signal) override {
        std::lock_guard lock(mutex_);
        if (failure_) return core::Result<void>::failure(*failure_);
        if (rendezvous_.remote_identity.empty()) return core::Result<void>::failure(error(1524, "Relay introduction is not paired"));
        if (signal.empty() || signal.size() > 65536) return core::Result<void>::failure(error(1521, "Invalid relay signal size"));
        if (outbound_.size() >= options_.queue_frames || signal.size() + 2 > options_.queue_bytes - outbound_bytes_)
            return core::Result<void>::failure(error(1526, "Relay client send queue is full"));
        auto bytes = message(Kind::signal); bytes.insert(bytes.end(), signal.begin(), signal.end());
        outbound_bytes_ += bytes.size(); outbound_.push_back(std::move(bytes));
        return core::Result<void>::success();
    }
    core::Result<std::optional<GnsSignal>> poll() override {
        std::lock_guard lock(mutex_);
        if (failure_) return core::Result<std::optional<GnsSignal>>::failure(*failure_);
        if (inbound_.empty()) return core::Result<std::optional<GnsSignal>>::success({});
        auto signal = std::move(inbound_.front()); inbound_.pop_front(); inbound_bytes_ -= signal.bytes.size();
        return core::Result<std::optional<GnsSignal>>::success(std::move(signal));
    }
private:
    NetworkIntroductionOptions options_;
    Bytes request_;
    std::string expected_identity_, expected_code_, alias_;
    Clock::time_point expires_{};
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    GnsRendezvous rendezvous_;
    std::optional<GnsTurnCredentials> turn_, refreshed_;
    std::optional<GnsSessionLease> lease_;
    bool renew_wanted_{};
    unsigned renew_attempts_{};
    Clock::time_point renew_retry_{};
    std::optional<core::Diagnostic> failure_;
    std::string publisher_ownership_;
    bool admitted_{}, close_requested_{}, close_sent_{}, publication_closed_{};
    std::deque<Bytes> outbound_;
    std::deque<GnsSignal> inbound_;
    std::size_t outbound_bytes_{}, inbound_bytes_{};
    std::jthread worker_;
    void fail(core::Diagnostic diagnostic) {
        std::lock_guard lock(mutex_);
        if (!failure_) failure_ = std::move(diagnostic);
        condition_.notify_all();
    }
    void response(const Bytes& bytes) {
        std::lock_guard lock(mutex_);
        auto malformed = [&] { failure_ = error(1521, "Malformed relay service response"); };
        if (bytes.size() < 2 || bytes[0] != 2) { malformed(); condition_.notify_all(); return; }
        Reader reader{std::span(bytes).subspan(2)};
        const auto kind = static_cast<Kind>(bytes[1]);
        if (kind == Kind::error) {
            const auto number = reader.u32();
            if (!reader.done() || !service_error_number(number)) malformed();
            else failure_ = error(number, "Relay service rejected the request or ended the session");
        } else if (kind == Kind::healthy && reader.done() && request_[1] == static_cast<std::uint8_t>(Kind::health)) {
            admitted_ = true;
        } else if (kind == Kind::published && !admitted_ && request_[1] == static_cast<std::uint8_t>(Kind::publish)) {
            rendezvous_.session = reader.string(32); rendezvous_.local_identity = reader.string(gns_identity_max_bytes);
            alias_ = reader.string(4);
            const auto lifetime = reader.u32();
            publisher_ownership_ = reader.string(32);
            expires_ = Clock::now() + std::chrono::milliseconds(lifetime);
            if ((!alias_.empty() && !alias_valid(alias_)) || lifetime == 0 || lifetime > 86400000 || !reader.done() || !code_valid(rendezvous_.session) || !code_valid(publisher_ownership_) || publisher_ownership_ == rendezvous_.session || rendezvous_.local_identity != expected_identity_) malformed();
            else admitted_ = true;
        } else if (kind == Kind::paired && rendezvous_.remote_identity.empty()) {
            GnsRendezvous r{reader.string(32), reader.string(gns_identity_max_bytes), reader.string(gns_identity_max_bytes)};
            GnsTurnCredentials turn{reader.string(512), reader.string(256), reader.string(256), std::chrono::seconds(reader.u32())};
            if (!reader.done() || !code_valid(r.session) || !identity_valid(r.local_identity) || !identity_valid(r.remote_identity)
                || r.local_identity == r.remote_identity || !turn_uri_list_valid(turn.uri)
                || r.local_identity != expected_identity_ || (code_valid(expected_code_) && r.session != expected_code_)
                || (!rendezvous_.session.empty() && (rendezvous_.session != r.session || rendezvous_.local_identity != r.local_identity))
                || (!turn.uri.empty() && (turn.username.empty() || turn.password.empty() || turn.lifetime.count() <= 0))) malformed();
            else { rendezvous_ = std::move(r); if (!turn.uri.empty()) turn_ = std::move(turn); admitted_ = true; }
        } else if (kind == Kind::expiring && !rendezvous_.remote_identity.empty()) {
            const auto warning = parse_expiring(reader);
            if (!warning) malformed();
            else {
                const auto now = Clock::now();
                GnsSessionLease next = lease_.value_or(GnsSessionLease{});
                next.ends = now + warning->remaining; next.hard_ends = now + warning->hard_remaining; next.renewable = warning->renewable;
                lease_ = std::move(next);
                if (warning->renewable && renew_attempts_ < 6) { renew_wanted_ = true; renew_retry_ = now; }
            }
        } else if (kind == Kind::renewed && !rendezvous_.remote_identity.empty()) {
            const auto renewed = parse_renewed(reader);
            if (!renewed) malformed();
            else {
                const auto now = Clock::now();
                GnsSessionLease next = lease_.value_or(GnsSessionLease{});
                next.ends = now + renewed->remaining; next.hard_ends = now + renewed->hard_remaining;
                next.renewable = renewed->hard_remaining > renewed->remaining; ++next.renewals; next.last_refusal.clear();
                lease_ = std::move(next); renew_wanted_ = false; renew_attempts_ = 0;
                if (!renewed->turn.uri.empty()) refreshed_ = renewed->turn;
            }
        } else if (kind == Kind::renew_refused && !rendezvous_.remote_identity.empty()) {
            const auto refusal = parse_refused(reader);
            if (!refusal) malformed();
            else if (refusal->reason == renew_refused_maximum) {
                renew_wanted_ = false;
                if (lease_) { lease_->renewable = false; lease_->last_refusal = "EAWR-NET-1732"; }
            } else if (refusal->reason == renew_refused_credentials) {
                // Transient: the service keeps the lease as it was. Ask again shortly, inside the service's budgets.
                renew_wanted_ = renew_attempts_ < 6; renew_retry_ = Clock::now() + options_.renewal_retry;
                if (lease_) lease_->last_refusal = "EAWR-NET-1733";
            } else renew_wanted_ = false; // Early: the other peer's renewal is already in progress.
        } else if (kind == Kind::revoked && close_requested_ && rendezvous_.remote_identity.empty() && reader.done()) {
            publication_closed_ = true;
        } else if (kind == Kind::forwarded && !rendezvous_.remote_identity.empty()) {
            auto from = reader.string(gns_identity_max_bytes);
            if (!reader.valid || from != rendezvous_.remote_identity || reader.bytes.empty() || reader.bytes.size() > 65536) malformed();
            else if (inbound_.size() >= options_.queue_frames || reader.bytes.size() > options_.queue_bytes - inbound_bytes_)
                failure_ = error(1526, "Relay client receive queue is full");
            else { inbound_bytes_ += reader.bytes.size(); inbound_.push_back({std::move(from), {reader.bytes.begin(), reader.bytes.end()}}); }
        } else malformed();
        condition_.notify_all();
    }
    void run(std::stop_token stop) {
        const auto deadline = Clock::now() + options_.timeout;
        auto ctx = client_context(options_.ca_file, options_.certificate_sha256);
        if (!ctx) { fail(ctx.error()); return; }
        const auto handle = connect_service(options_.address, options_.port, deadline, stop);
        if (handle == socket::invalid) { fail(error(1519, "Relay service connection failed")); return; }
        TlsStream stream(std::move(ctx).value(), handle, false, options_.certificate_name,
            options_.certificate_sha256, options_.queue_bytes, options_.queue_frames);
        if (!stream.queue(request_)) { fail(error(1526, "Relay initial queue failed")); return; }
        auto heartbeat = Clock::now();
        while (!stop.stop_requested()) {
            stream.pump();
            if (auto bytes = stream.receive()) response(numeric_turn(std::move(*bytes), stop));
            {
                std::lock_guard lock(mutex_);
                if (failure_ || publication_closed_) return;
                if (admitted_ && request_[1] == static_cast<std::uint8_t>(Kind::health)) return;
                if (!admitted_ && Clock::now() >= deadline) { failure_ = error(1528, "Relay introduction request timed out"); condition_.notify_all(); return; }
                if (close_requested_ && !close_sent_ && stream.queue(message(Kind::revoke))) close_sent_ = true;
                // One bounded owner performs all TLS I/O; GNS-thread callbacks
                // never block on the network and never share an SSL object.
                if (!outbound_.empty() && stream.queue(outbound_.front())) {
                    outbound_bytes_ -= outbound_.front().size(); outbound_.pop_front();
                }
                if (renew_wanted_ && renew_attempts_ < 6 && lease_ && Clock::now() < lease_->ends
                    && Clock::now() >= renew_retry_ && stream.queue(message(Kind::renew))) {
                    renew_wanted_ = false; ++renew_attempts_;
                }
                if (admitted_ && Clock::now() - heartbeat >= std::chrono::seconds(5)) {
                    if (stream.queue(message(Kind::ping))) heartbeat = Clock::now();
                }
            }
            if (stream.failed()) { fail(stream.failure().value_or(error(stream.ready() ? 1528 : 1519, "Relay TLS connection failed"))); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
};
core::Result<std::shared_ptr<NetworkEndpoint>> open(NetworkIntroductionOptions options, Bytes request) {
    const bool publisher = request[1] == static_cast<std::uint8_t>(Kind::publish);
    if (!valid(options, !publisher)) return core::Result<std::shared_ptr<NetworkEndpoint>>::failure(error(1519, "Invalid relay client options"));
    auto result = std::make_shared<NetworkEndpoint>(std::move(options), std::move(request));
    result->start(); auto ready = result->wait();
    if (!ready) return core::Result<std::shared_ptr<NetworkEndpoint>>::failure(ready.error());
    return core::Result<std::shared_ptr<NetworkEndpoint>>::success(std::move(result));
}
}
core::Result<GnsIntroduction> NetworkGnsIntroduction::publish(std::string identity, std::chrono::milliseconds lifetime) {
    if (!identity_valid(identity) || lifetime.count() <= 0 || lifetime > std::chrono::hours(24))
        return core::Result<GnsIntroduction>::failure(error(1519, "Invalid relay publish request"));
    auto request = message(Kind::publish); string(request, options_.access_token); string(request, identity);
    u32(request, static_cast<std::uint32_t>(lifetime.count())); u32(request, options_.public_alias ? 1 : 0);
    auto result = open(options_, std::move(request));
    if (!result) return core::Result<GnsIntroduction>::failure(result.error());
    auto endpoint = std::move(result).value();
    return core::Result<GnsIntroduction>::success({endpoint->rendezvous().session, endpoint, endpoint->alias(), endpoint->expires(), endpoint->ownership()});
}
core::Result<std::shared_ptr<GnsSignallingEndpoint>> NetworkGnsIntroduction::join(std::string code, std::string identity) {
    if (!identity_valid(identity) || (!code_valid(code) && !alias_valid(code)))
        return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(error(1519, "Invalid relay join request"));
    auto request = message(Kind::join); string(request, code); string(request, identity);
    auto result = open(options_, std::move(request));
    if (!result) return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::failure(result.error());
    return core::Result<std::shared_ptr<GnsSignallingEndpoint>>::success(std::move(result).value());
}
core::Result<void> NetworkGnsIntroduction::health(NetworkIntroductionOptions options) {
    auto result = open(std::move(options), message(Kind::health));
    if (!result) return core::Result<void>::failure(result.error());
    return core::Result<void>::success();
}

struct NetworkOnlineLobby::Impl {
    NetworkIntroductionOptions options;
    std::string principal;
    mutable std::mutex mutex, principal_mutex;
    std::string published_principal;
    std::unique_ptr<TlsStream> stream;
    std::optional<core::Diagnostic> failure;
    // Menu channel (#3758): a connection of its own, so a relay without the channel (it closes the connection that sent
    // the unknown kind) never costs the directory connection.
    std::mutex menu_mutex;
    std::unique_ptr<TlsStream> menu_stream;
    Impl(NetworkIntroductionOptions o, std::string resume) : options(std::move(o)), principal(std::move(resume)), published_principal(principal) {}
    void set_principal(std::string value) {
        principal = std::move(value);
        std::lock_guard lock(principal_mutex); published_principal = principal;
    }
    core::Result<Bytes> wire(Bytes request) {
        if (!stream->queue(std::move(request))) return core::Result<Bytes>::failure(error(1526, "Lobby request queue full"));
        const auto deadline = Clock::now() + options.timeout;
        auto transient = error(1552, "Lobby response timed out");
        while (Clock::now() < deadline) {
            stream->pump();
            if (auto reply = stream->receive()) {
                if (reply->size() < 2 || (*reply)[0] != 2) { failure = error(1540, "Malformed lobby response"); break; }
                if ((*reply)[1] == static_cast<std::uint8_t>(Kind::error)) {
                    Reader reader{std::span(*reply).subspan(2)}; const auto code = reader.u32();
                    if (!reader.done() || !room_error_number(code)) { failure = error(1540, "Malformed lobby refusal"); break; }
                    if (code == 1519 || code == 1525 || code == 1526 || code == 1527 || code == 1528) stream.reset();
                    return core::Result<Bytes>::failure(error(code, "Lobby service refused the request"));
                }
                return core::Result<Bytes>::success(std::move(*reply));
            }
            if (stream->failed()) { transient = stream->failure().value_or(error(1519, "Lobby TLS connection failed")); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        stream.reset();
        return core::Result<Bytes>::failure(failure ? *failure : transient);
    }
    core::Result<Bytes> exchange(Bytes request) {
        std::lock_guard lock(mutex);
        if (failure) return core::Result<Bytes>::failure(*failure);
        if (!stream) {
            if (!valid(options, true) || (!principal.empty() && !code_valid(principal)))
                return core::Result<Bytes>::failure(error(1519, "Invalid lobby client configuration"));
            auto context = client_context(options.ca_file, options.certificate_sha256);
            if (!context) return core::Result<Bytes>::failure(context.error());
            const auto handle = connect_service(options.address, options.port, Clock::now() + options.timeout);
            if (handle == socket::invalid) return core::Result<Bytes>::failure(error(1519, "Lobby endpoint failed"));
            stream = std::make_unique<TlsStream>(std::move(context).value(), handle, false, options.certificate_name,
                options.certificate_sha256, options.queue_bytes, options.queue_frames);
            auto opening = message(Kind::directory_open); string(opening, principal);
            auto opened = wire(std::move(opening));
            if (!opened) {
                stream.reset();
                if (opened.error().code == "EAWR-NET-1541") set_principal({});
                return opened;
            }
            Reader reader{std::span(opened.value()).subspan(2)}; auto received = reader.string(32);
            if (opened.value()[1] != static_cast<std::uint8_t>(Kind::directory_opened) || !reader.done()
                || !code_valid(received) || (!principal.empty() && received != principal)) {
                failure = error(1540, "Malformed lobby principal response"); stream.reset();
                return core::Result<Bytes>::failure(*failure);
            }
            set_principal(std::move(received));
        }
        return wire(std::move(request));
    }
    // One request on the menu connection; `opening` replaces the connection first. A transport failure or a budget refusal
    // that ends the connection drops the stream, so the caller's next menu_enter starts over.
    core::Result<Bytes> menu_exchange(Bytes request, bool opening) {
        std::lock_guard lock(menu_mutex);
        if (opening) {
            menu_stream.reset();
            if (!valid(options, true)) return core::Result<Bytes>::failure(error(1519, "Invalid lobby client configuration"));
            auto context = client_context(options.ca_file, options.certificate_sha256);
            if (!context) return core::Result<Bytes>::failure(context.error());
            const auto handle = connect_service(options.address, options.port, Clock::now() + options.timeout);
            if (handle == socket::invalid) return core::Result<Bytes>::failure(error(1519, "Lobby endpoint failed"));
            menu_stream = std::make_unique<TlsStream>(std::move(context).value(), handle, false, options.certificate_name,
                options.certificate_sha256, options.queue_bytes, options.queue_frames);
        } else if (!menu_stream) return core::Result<Bytes>::failure(error(1519, "Lobby chat connection is closed"));
        if (!menu_stream->queue(std::move(request))) { menu_stream.reset(); return core::Result<Bytes>::failure(error(1526, "Lobby request queue full")); }
        const auto deadline = Clock::now() + options.timeout;
        auto transient = error(1552, "Lobby response timed out");
        while (Clock::now() < deadline) {
            menu_stream->pump();
            if (auto reply = menu_stream->receive()) {
                if (reply->size() < 2 || (*reply)[0] != 2) { transient = error(1540, "Malformed lobby response"); break; }
                if ((*reply)[1] == static_cast<std::uint8_t>(Kind::error)) {
                    Reader reader{std::span(*reply).subspan(2)}; const auto code = reader.u32();
                    if (!reader.done() || !room_error_number(code)) { transient = error(1540, "Malformed lobby refusal"); break; }
                    // 1521 on open: the relay predates the channel and closes this connection. 1525..1528 end it too.
                    if (opening || (code >= 1519 && code <= 1528)) menu_stream.reset();
                    return core::Result<Bytes>::failure(error(code, "Lobby service refused the request"));
                }
                return core::Result<Bytes>::success(std::move(*reply));
            }
            if (menu_stream->failed()) { transient = menu_stream->failure().value_or(error(1519, "Lobby TLS connection failed")); break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        menu_stream.reset();
        return core::Result<Bytes>::failure(std::move(transient));
    }
    void menu_leave() { std::lock_guard lock(menu_mutex); menu_stream.reset(); }
    void latch_protocol(core::Diagnostic diagnostic) {
        std::lock_guard lock(mutex); failure = std::move(diagnostic); stream.reset();
    }
    core::Result<void> ack(Bytes request) {
        auto reply = exchange(std::move(request));
        if (!reply) return core::Result<void>::failure(reply.error());
        if (reply.value() != message(Kind::directory_ok)) {
            auto diagnostic = error(1540, "Malformed lobby acknowledgement"); latch_protocol(diagnostic);
            return core::Result<void>::failure(std::move(diagnostic));
        }
        return core::Result<void>::success();
    }
};
NetworkOnlineLobby::NetworkOnlineLobby(NetworkIntroductionOptions options, std::string principal)
    : impl_(std::make_unique<Impl>(std::move(options), std::move(principal))) {}
NetworkOnlineLobby::~NetworkOnlineLobby() = default;
std::string NetworkOnlineLobby::principal() const { std::lock_guard lock(impl_->principal_mutex); return impl_->published_principal; }
core::Result<OnlineRoomPage> NetworkOnlineLobby::list(std::string after, std::uint32_t limit) {
    if ((!after.empty() && !code_valid(after)) || limit == 0 || limit > 50)
        return core::Result<OnlineRoomPage>::failure(error(1540, "Invalid room page request"));
    auto request = message(Kind::directory_list); string(request, after); u32(request, limit);
    auto reply = impl_->exchange(std::move(request));
    if (!reply) return core::Result<OnlineRoomPage>::failure(reply.error());
    auto decoded = room_page(reply.value()); if (!decoded) impl_->latch_protocol(decoded.error()); return decoded;
}
core::Result<OnlineRoomPublication> NetworkOnlineLobby::publish(OnlineRoomMetadata value, std::string password) {
    return publish_bridge(std::move(value), std::move(password), {}, {});
}
core::Result<OnlineRoomPublication> NetworkOnlineLobby::publish_bridge(OnlineRoomMetadata value, std::string password, std::string invitation, std::string ownership) {
    value.name = clean_text(value.name, 24); value.host_name = clean_text(value.host_name, 20);
    if (!metadata_valid(value) || password.size() > 128 || impl_->options.access_token.size() > 256
        || (!invitation.empty() && (!code_valid(invitation) || !code_valid(ownership))))
        return core::Result<OnlineRoomPublication>::failure(error(1540, "Invalid room publication"));
    auto request = message(Kind::directory_publish); string(request, impl_->options.access_token); metadata(request, value); string(request, password);
    string(request, invitation); string(request, ownership);
    auto reply = impl_->exchange(std::move(request));
    if (!reply) return core::Result<OnlineRoomPublication>::failure(reply.error());
    Reader reader{std::span(reply.value()).subspan(2)};
    OnlineRoomPublication result{reader.string(32), reader.string(32), reader.u32()};
    if (reply.value()[1] != static_cast<std::uint8_t>(Kind::directory_published) || !reader.done()
        || !code_valid(result.room) || !code_valid(result.ownership) || result.room == result.ownership || result.lease_ms != 30000)
    {
        auto diagnostic = error(1540, "Malformed room publication"); impl_->latch_protocol(diagnostic);
        return core::Result<OnlineRoomPublication>::failure(std::move(diagnostic));
    }
    return core::Result<OnlineRoomPublication>::success(std::move(result));
}
namespace {
Bytes owner_request(Kind kind, const OnlineRoomPublication& publication) {
    auto request = message(kind); string(request, publication.room); string(request, publication.ownership); return request;
}
bool owner_valid(const OnlineRoomPublication& publication) { return code_valid(publication.room) && code_valid(publication.ownership); }
}
core::Result<void> NetworkOnlineLobby::heartbeat(const OnlineRoomPublication& publication, const OnlineRoomMetadata& input) {
    auto value = input; value.name = clean_text(value.name, 24); value.host_name = clean_text(value.host_name, 20);
    if (!owner_valid(publication) || !metadata_valid(value)) return core::Result<void>::failure(error(1540, "Invalid room heartbeat"));
    auto request = owner_request(Kind::directory_heartbeat, publication); metadata(request, value); return impl_->ack(std::move(request));
}
core::Result<void> NetworkOnlineLobby::remove(const OnlineRoomPublication& publication) {
    if (!owner_valid(publication)) return core::Result<void>::failure(error(1540, "Invalid room removal"));
    return impl_->ack(owner_request(Kind::directory_remove, publication));
}
core::Result<OnlineRoomAdmission> NetworkOnlineLobby::join(std::string room, std::string password, std::string name) {
    name = clean_text(name, 20);
    if (!code_valid(room) || password.size() > 128 || name.empty()) return core::Result<OnlineRoomAdmission>::failure(error(1540, "Invalid room admission"));
    auto request = message(Kind::directory_join); string(request, room); string(request, password); string(request, name);
    auto reply = impl_->exchange(std::move(request));
    if (!reply) return core::Result<OnlineRoomAdmission>::failure(reply.error());
    Reader reader{std::span(reply.value()).subspan(2)}; OnlineRoomAdmission result{reader.string(32), reader.u32(), reader.string(32)};
    // A room with an open signalling scope appends its reserved slot and member capability (slice N3).
    if (reader.valid && !reader.bytes.empty()) { result.slot = reader.u32(); result.member_capability = reader.string(32); }
    if (reply.value()[1] != static_cast<std::uint8_t>(Kind::directory_admitted) || !reader.done() || result.room != room || result.seat < 2
        || (!result.bridge_invitation.empty() && !code_valid(result.bridge_invitation))
        || (!result.member_capability.empty() && (!code_valid(result.member_capability) || result.slot == 0 || result.slot >= max_match_seats))
        || (result.member_capability.empty() && result.slot != 0))
    {
        auto diagnostic = error(1540, "Malformed room admission"); impl_->latch_protocol(diagnostic);
        return core::Result<OnlineRoomAdmission>::failure(std::move(diagnostic));
    }
    return core::Result<OnlineRoomAdmission>::success(std::move(result));
}
core::Result<void> NetworkOnlineLobby::leave() { return impl_->ack(message(Kind::directory_leave)); }
core::Result<void> NetworkOnlineLobby::send_chat(std::string text) {
    if (text.size() > 128 || clean_text(text, 128).empty()) return core::Result<void>::failure(error(1540, "Invalid room chat"));
    auto request = message(Kind::directory_chat); string(request, text); return impl_->ack(std::move(request));
}
core::Result<OnlineChatPage> NetworkOnlineLobby::chat(std::uint32_t after) {
    auto request = message(Kind::directory_poll); u32(request, after); auto reply = impl_->exchange(std::move(request));
    if (!reply) return core::Result<OnlineChatPage>::failure(reply.error());
    auto decoded = chat_page(reply.value(), after); if (!decoded) impl_->latch_protocol(decoded.error()); return decoded;
}
core::Result<OnlineMenuPage> NetworkOnlineLobby::menu_enter(std::string display_name) {
    display_name = clean_text(display_name, 20);
    if (display_name.empty()) return core::Result<OnlineMenuPage>::failure(error(1540, "Invalid lobby chat name"));
    auto request = message(Kind::menu_open); string(request, display_name);
    auto reply = impl_->menu_exchange(std::move(request), true);
    if (!reply) return core::Result<OnlineMenuPage>::failure(reply.error());
    auto decoded = menu_page_decode(reply.value(), 0);
    if (!decoded || !decoded.value().roster_present) { impl_->menu_leave(); return core::Result<OnlineMenuPage>::failure(error(1540, "Malformed lobby chat page")); }
    return decoded;
}
core::Result<void> NetworkOnlineLobby::menu_say(std::string text) {
    if (text.size() > 128 || clean_text(text, 128).empty()) return core::Result<void>::failure(error(1540, "Invalid lobby chat line"));
    auto request = message(Kind::menu_chat); string(request, text);
    auto reply = impl_->menu_exchange(std::move(request), false);
    if (!reply) return core::Result<void>::failure(reply.error());
    auto decoded = menu_page_decode(reply.value(), 0);
    if (!decoded || !decoded.value().lines.empty() || decoded.value().roster_present) {
        impl_->menu_leave(); return core::Result<void>::failure(error(1540, "Malformed lobby chat acknowledgement"));
    }
    return core::Result<void>::success();
}
core::Result<OnlineMenuPage> NetworkOnlineLobby::menu_poll(std::uint32_t after, std::uint32_t roster_version) {
    auto request = message(Kind::menu_poll); u32(request, after); u32(request, roster_version);
    auto reply = impl_->menu_exchange(std::move(request), false);
    if (!reply) return core::Result<OnlineMenuPage>::failure(reply.error());
    auto decoded = menu_page_decode(reply.value(), after);
    if (!decoded) { impl_->menu_leave(); return core::Result<OnlineMenuPage>::failure(decoded.error()); }
    return decoded;
}
void NetworkOnlineLobby::menu_leave() { impl_->menu_leave(); }
core::Result<void> NetworkOnlineLobby::kick(const OnlineRoomPublication& publication, std::uint32_t seat) {
    if (!owner_valid(publication) || seat < 2) return core::Result<void>::failure(error(1540, "Invalid room chat removal"));
    auto request = owner_request(Kind::directory_kick, publication); u32(request, seat); return impl_->ack(std::move(request));
}
#endif
} // namespace eawr::net
