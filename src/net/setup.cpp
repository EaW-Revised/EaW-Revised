#include "eawr/net/setup.hpp"
#include "eawr/net/galactic_setup.hpp"
#include "eawr/net/lan_directory.hpp"
#include "eawr/core/sha256.hpp"

#include <algorithm>
#include <deque>
#include <set>

namespace eawr::net {
namespace {
using Bytes = std::vector<std::uint8_t>;
template<class T> core::Result<T> error(SetupReason reason, std::string message) {
    return core::Result<T>::failure(SetupRefusal{reason, {}, std::move(message), {}, {}}.diagnostic());
}
void word(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (i * 8U)));
}
void blob(Bytes& bytes, std::span<const std::uint8_t> value) {
    word(bytes, value.size(), 4);
    bytes.insert(bytes.end(), value.begin(), value.end());
}
void text(Bytes& bytes, std::string_view value) {
    blob(bytes, {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()});
}
struct Reader {
    std::span<const std::uint8_t> bytes;
    bool word(std::uint64_t& value, unsigned width) {
        if (bytes.size() < width) return false;
        value = 0;
        for (unsigned i = 0; i < width; ++i) value |= static_cast<std::uint64_t>(bytes[i]) << (i * 8U);
        bytes = bytes.subspan(width);
        return true;
    }
    bool blob(std::span<const std::uint8_t>& value, std::size_t limit) {
        std::uint64_t size{};
        if (!word(size, 4) || size > limit || size > bytes.size()) return false;
        value = bytes.first(static_cast<std::size_t>(size));
        bytes = bytes.subspan(static_cast<std::size_t>(size));
        return true;
    }
    bool text(std::string& value, std::size_t limit, bool display = false) {
        std::span<const std::uint8_t> data;
        if (!blob(data, limit)) return false;
        const std::string_view raw(reinterpret_cast<const char*>(data.data()), data.size());
        const auto clean = sanitize_lan_text(raw, limit);
        // Hashed fields must remain byte-exact; only display text is repaired.
        if (!display && clean != raw) return false;
        value = clean;
        return true;
    }
};
bool valid_setup_text(const sim::tactical::TacticalSetup& setup) {
    if (!setup.skirmish) return false;
    const auto valid = [](std::string_view value) { return sanitize_lan_text(value, 1024) == value; };
    const auto& metadata = *setup.skirmish;
    if (!valid(metadata.map) || !valid(metadata.map_sha256) || !valid(metadata.match.win_condition)
        || !valid(metadata.match.space_win_condition)) return false;
    for (const auto& slot : metadata.slots)
        for (const auto& name : slot.fleet) if (!valid(name)) return false;
    return true;
}
bool hash_text(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
core::Result<void> validate_offer(const SetupOffer& offer) {
    const bool galactic = offer.domain == galactic_setup_domain;
    if ((offer.domain != tactical_setup_domain && !galactic) || offer.setup.empty() || offer.setup.size() > max_setup_bytes
        || offer.input_delay == 0 || offer.input_delay > max_input_delay || offer.seats.size() < 2
        || offer.final_tick_count == 0 || offer.final_tick_count > max_session_ticks
        || offer.seats.size() > max_match_seats || offer.identity.build.empty() || offer.identity.build.size() > 256
        || sanitize_lan_text(offer.identity.build, 256) != offer.identity.build)
        return error<void>(SetupReason::options, "unsupported setup domain, size, delay, roster or build identity");
    if (offer.recovery.grace_ms == 0 || offer.recovery.grace_ms > max_recovery_timeout_ms
        || offer.recovery.progress_timeout_ms == 0 || offer.recovery.progress_timeout_ms > max_recovery_timeout_ms
        || offer.recovery.hard_cap_ms > max_recovery_timeout_ms
        || offer.recovery.hard_cap_ms < offer.recovery.grace_ms || offer.recovery.hard_cap_ms < offer.recovery.progress_timeout_ms
        || offer.recovery.max_log_bytes < offer.setup.size() + 1024 || offer.recovery.max_log_bytes > 256U * 1024U * 1024U
        || offer.recovery.resync_timeout_ms == 0 || offer.recovery.resync_timeout_ms > max_recovery_timeout_ms)
        return error<void>(SetupReason::options, "invalid recovery policy limits");
    if (auto valid = sim::validate_match_identity(offer.identity.content); !valid)
        return error<void>(SetupReason::options, valid.error().message);
    if (galactic) return validate_galactic_offer(offer);
    auto setup = decode_skirmish_setup(offer.setup);
    if (!setup) return core::Result<void>::failure(setup.error());
    if (setup.value().seed != offer.seed || !setup.value().match_identity
        || *setup.value().match_identity != offer.identity.content)
        return error<void>(SetupReason::options, "setup seed or content identity disagrees with offer");
    std::set<sim::SeatId> unique;
    std::set<sim::tactical::PlayerId> humans;
    for (std::size_t i = 0; i < offer.seats.size(); ++i) {
        const auto& seat = offer.seats[i];
        if (seat.peer != i || seat.seat.value == 0 || !unique.insert(seat.seat).second)
            return error<void>(SetupReason::options, "peer indices or seat IDs are not canonical");
        const auto& slots = setup.value().skirmish->slots;
        if (std::none_of(slots.begin(), slots.end(), [&](const auto& slot) {
                return slot.player == seat.player && slot.human;
            })) return error<void>(SetupReason::options, "seat is not bound to a human skirmish slot");
        if (!humans.insert(seat.player).second)
            return error<void>(SetupReason::options, "skirmish human seats must have distinct owners");
    }
    for (const auto& slot : setup.value().skirmish->slots)
        if (slot.human && !humans.contains(slot.player))
            return error<void>(SetupReason::options, "human skirmish slot has no seat");
    std::set<sim::tactical::TeamId> human_teams;
    for (const auto player : humans) {
        const auto found = std::find_if(setup.value().players.begin(), setup.value().players.end(),
            [player](const auto& value) { return value.player_id == player; });
        if (found == setup.value().players.end() || !found->commandable())
            return error<void>(SetupReason::options, "human seat owner is not commandable");
        human_teams.insert(found->team_id);
    }
    if (human_teams.size() == 1 && std::none_of(setup.value().skirmish->slots.begin(),
            setup.value().skirmish->slots.end(), [&](const auto& slot) {
                return !slot.human && std::any_of(setup.value().players.begin(), setup.value().players.end(),
                    [&](const auto& player) { return player.player_id == slot.player
                        && !human_teams.contains(player.team_id); });
            }))
        return error<void>(SetupReason::options, "skirmish requires at least two opposing teams, including AI");
    return core::Result<void>::success();
}
core::Result<sim::MatchIdentity> read_identity(std::span<const std::uint8_t> bytes,
    std::span<const std::uint8_t> digest) {
    const auto separator = sim::match_identity_encoding;
    if (bytes.size() < separator.size() || !std::equal(separator.begin(), separator.end(), bytes.begin()))
        return error<sim::MatchIdentity>(SetupReason::malformed, "invalid identity separator");
    Reader reader{bytes.subspan(separator.size())};
    sim::MatchIdentity identity;
    std::uint64_t version{}, count{};
    if (!reader.word(version, 4) || !reader.word(count, 4) || count > 64)
        return error<sim::MatchIdentity>(SetupReason::malformed, "invalid identity count");
    identity.version = static_cast<std::uint32_t>(version);
    for (std::uint64_t i = 0; i < count; ++i) {
        sim::IdentityDomain domain;
        if (!reader.text(domain.name, 128) || !reader.word(version, 4) || reader.bytes.size() < 32)
            return error<sim::MatchIdentity>(SetupReason::malformed, "truncated identity domain");
        domain.version = static_cast<std::uint32_t>(version);
        std::copy_n(reader.bytes.begin(), 32, domain.digest.begin());
        reader.bytes = reader.bytes.subspan(32);
        identity.domains.push_back(std::move(domain));
    }
    if (!reader.bytes.empty() || digest.size() != 32)
        return error<sim::MatchIdentity>(SetupReason::malformed, "trailing identity bytes");
    std::copy(digest.begin(), digest.end(), identity.sha256.begin());
    if (auto valid = sim::validate_match_identity(identity); !valid)
        return error<sim::MatchIdentity>(SetupReason::malformed, valid.error().message);
    return core::Result<sim::MatchIdentity>::success(std::move(identity));
}
}

core::Diagnostic SetupRefusal::diagnostic() const {
    core::Diagnostic result;
    result.code = "EAWR-NET-030" + std::to_string(static_cast<unsigned>(reason));
    result.message = message;
    // Preserve reserved credential diagnostics in the existing refusal detail.
    if (message.size() >= 15 && message.starts_with("EAWR-NET-102") && message[12] >= '0' && message[12] <= '9'
        && message.substr(13, 2) == ": ") {
        result.code = message.substr(0, 13); result.message = message.substr(15);
    }
    return result;
}
core::Result<Bytes> encode_skirmish_setup(const sim::tactical::TacticalSetup& setup) {
    if (!valid_setup_text(setup))
        return error<Bytes>(SetupReason::options, "skirmish setup requires SKSU metadata with valid UTF-8 text");
    auto result = sim::tactical::write_replay({setup, 0, {}});
    if (!result) return error<Bytes>(SetupReason::options, result.error().message);
    if (result.value().size() > max_setup_bytes - 16384)
        return error<Bytes>(SetupReason::options, "skirmish setup exceeds offer budget");
    return result;
}
core::Result<sim::tactical::TacticalSetup> decode_skirmish_setup(std::span<const std::uint8_t> bytes) {
    using Result = core::Result<sim::tactical::TacticalSetup>;
    if (bytes.empty() || bytes.size() > max_setup_bytes)
        return error<sim::tactical::TacticalSetup>(SetupReason::malformed, "invalid skirmish setup size");
    auto replay = sim::tactical::parse_replay(bytes);
    if (!replay || !replay.value().setup.skirmish || replay.value().final_tick_count != 0
        || !replay.value().commands.empty())
        return error<sim::tactical::TacticalSetup>(SetupReason::malformed, "setup must be a command-free SKSU replay");
    if (!valid_setup_text(replay.value().setup))
        return error<sim::tactical::TacticalSetup>(SetupReason::malformed, "invalid UTF-8 in skirmish setup text");
    auto encoded = sim::tactical::write_replay(replay.value());
    if (!encoded || !std::equal(bytes.begin(), bytes.end(), encoded.value().begin(), encoded.value().end()))
        return error<sim::tactical::TacticalSetup>(SetupReason::malformed, "noncanonical skirmish setup bytes");
    return Result::success(std::move(replay).value().setup);
}
core::Result<Bytes> encode_setup_offer(const SetupOffer& offer) {
    if (auto valid = validate_offer(offer); !valid) return core::Result<Bytes>::failure(valid.error());
    Bytes bytes{setup_offer_kind};
    word(bytes, 3, 4); word(bytes, offer.seed, 8); word(bytes, offer.input_delay, 8); word(bytes, offer.final_tick_count, 8);
    text(bytes, offer.domain); blob(bytes, offer.setup); word(bytes, offer.seats.size(), 4);
    for (const auto& seat : offer.seats) {
        word(bytes, seat.peer, 4); word(bytes, seat.seat.value, 4); word(bytes, seat.player, 4);
    }
    text(bytes, offer.identity.build);
    blob(bytes, sim::canonical_match_identity(offer.identity.content));
    bytes.insert(bytes.end(), offer.identity.content.sha256.begin(), offer.identity.content.sha256.end());
    word(bytes, (offer.recovery.reconnect ? 1U : 0U) | (offer.recovery.resync ? 2U : 0U), 1);
    word(bytes, offer.recovery.grace_ms, 8); word(bytes, offer.recovery.progress_timeout_ms, 8);
    word(bytes, offer.recovery.hard_cap_ms, 8); word(bytes, offer.recovery.max_log_bytes, 8);
    word(bytes, offer.recovery.resync_timeout_ms, 8);
    const auto digest = core::sha256(bytes);
    bytes.insert(bytes.end(), digest.begin(), digest.end());
    if (bytes.size() > max_setup_bytes) return error<Bytes>(SetupReason::options, "offer exceeds byte limit");
    return core::Result<Bytes>::success(std::move(bytes));
}
core::Result<SetupOffer> decode_setup_offer(std::span<const std::uint8_t> frame) {
    using Result = core::Result<SetupOffer>;
    if (frame.size() < 65 || frame.size() > max_setup_bytes || frame.front() != setup_offer_kind)
        return error<SetupOffer>(SetupReason::malformed, "invalid offer kind or size");
    const auto body = frame.first(frame.size() - 32);
    const auto digest = core::sha256(body);
    if (!std::equal(digest.begin(), digest.end(), frame.end() - 32))
        return error<SetupOffer>(SetupReason::malformed, "offer integrity digest differs");
    Reader reader{body.subspan(1)};
    SetupOffer offer;
    std::uint64_t version{}, count{};
    std::span<const std::uint8_t> setup, identity;
    if (!reader.word(version, 4) || version != 3 || !reader.word(offer.seed, 8)
        || !reader.word(offer.input_delay, 8) || !reader.word(offer.final_tick_count, 8) || !reader.text(offer.domain, 128)
        || !reader.blob(setup, max_setup_bytes) || !reader.word(count, 4) || count < 2 || count > max_match_seats)
        return error<SetupOffer>(SetupReason::malformed, "truncated offer or unsupported version/count");
    offer.setup.assign(setup.begin(), setup.end());
    for (std::uint64_t i = 0; i < count; ++i) {
        std::uint64_t peer{}, seat{}, player{};
        if (!reader.word(peer, 4) || !reader.word(seat, 4) || !reader.word(player, 4))
            return error<SetupOffer>(SetupReason::malformed, "truncated seat");
        offer.seats.push_back({static_cast<PeerIndex>(peer), sim::SeatId{static_cast<std::uint32_t>(seat)},
            static_cast<sim::tactical::PlayerId>(player)});
    }
    if (!reader.text(offer.identity.build, 256) || !reader.blob(identity, 16384) || reader.bytes.size() != 73)
        return error<SetupOffer>(SetupReason::malformed, "truncated or trailing offer identity");
    auto parsed = read_identity(identity, reader.bytes.first(32));
    if (!parsed) return Result::failure(parsed.error());
    offer.identity.content = std::move(parsed).value();
    reader.bytes = reader.bytes.subspan(32);
    std::uint64_t flags{};
    if (!reader.word(flags, 1) || flags > 3 || !reader.word(offer.recovery.grace_ms, 8)
        || !reader.word(offer.recovery.progress_timeout_ms, 8) || !reader.word(offer.recovery.hard_cap_ms, 8)
        || !reader.word(offer.recovery.max_log_bytes, 8) || !reader.word(offer.recovery.resync_timeout_ms, 8))
        return error<SetupOffer>(SetupReason::malformed, "invalid recovery policy");
    offer.recovery.reconnect = (flags & 1U) != 0; offer.recovery.resync = (flags & 2U) != 0;
    if (auto valid = validate_offer(offer); !valid) return Result::failure(valid.error());
    return Result::success(std::move(offer));
}
core::Result<Bytes> encode_setup_refusal(const SetupRefusal& input) {
    auto refusal = input;
    refusal.domain = sanitize_lan_text(refusal.domain, 128);
    refusal.message = sanitize_lan_text(refusal.message, 1024);
    if (static_cast<unsigned>(refusal.reason) > 7 || refusal.domain.size() > 128
        || refusal.message.empty() || refusal.message.size() > 1024
        || (refusal.reason == SetupReason::tick_zero
            ? (!(refusal.local_hash.empty() && refusal.remote_hash.empty())
                && (!hash_text(refusal.local_hash) || !hash_text(refusal.remote_hash)))
            : (!refusal.local_hash.empty() || !refusal.remote_hash.empty()))
        || refusal.domain.find('\0') != std::string::npos || refusal.message.find('\0') != std::string::npos)
        return error<Bytes>(SetupReason::malformed, "invalid typed setup refusal");
    Bytes bytes{setup_refuse_kind, static_cast<std::uint8_t>(refusal.reason)};
    text(bytes, refusal.domain); text(bytes, refusal.message);
    text(bytes, refusal.local_hash); text(bytes, refusal.remote_hash);
    return core::Result<Bytes>::success(std::move(bytes));
}
core::Result<SetupRefusal> decode_setup_refusal(std::span<const std::uint8_t> frame) {
    if (frame.size() < 2 || frame.size() > 1408 || frame[0] != setup_refuse_kind || frame[1] > 7)
        return error<SetupRefusal>(SetupReason::malformed, "invalid refusal envelope");
    SetupRefusal refusal{static_cast<SetupReason>(frame[1]), {}, {}, {}, {}};
    Reader reader{frame.subspan(2)};
    if (!reader.text(refusal.domain, 128, true) || !reader.text(refusal.message, 1024, true)
        || !reader.text(refusal.local_hash, 64) || !reader.text(refusal.remote_hash, 64) || !reader.bytes.empty())
        return error<SetupRefusal>(SetupReason::malformed, "invalid refusal text");
    auto valid = encode_setup_refusal(refusal);
    if (!valid) return core::Result<SetupRefusal>::failure(valid.error());
    return core::Result<SetupRefusal>::success(std::move(refusal));
}

struct SetupRuntime final : SessionMonitor, MessageTransport {
    MessageTransport* transport{};
    SetupOffer offer;
    SetupProviders providers;
    SetupOptions options;
    SetupPhase phase{SetupPhase::awaiting_offer};
    std::optional<SetupRefusal> refusal;
    std::array<std::uint8_t, 32> digest{};
    std::vector<bool> accepted;
    std::vector<bool> previewed;
    std::unique_ptr<SessionAdapter> adapter;
    std::optional<LockstepSession> core;
    std::optional<std::uint64_t> since, last_now;
    unsigned timed_phase{}; // 0 human wait/ready, 1 offer delivery, 2 tick-zero barrier.
    bool zero_recorded{}, released{}, taken{}, start_requested{}, started{};
    std::deque<Received> held; // Bounded frames arriving while the UI reviews an offer.
    std::size_t held_bytes{};

    core::Result<void> send(PeerIndex to, std::span<const std::uint8_t> frame) override { return transport->send(to, frame); }
    std::optional<Received> poll() override {
        if (held.empty()) return transport->poll();
        auto frame = std::move(held.front()); held.pop_front();
        held_bytes -= frame.frame.size();
        return frame;
    }
    LinkState link(PeerIndex peer) const override { return transport->link(peer); }
    std::optional<LinkStats> link_stats(PeerIndex peer) const override { return transport->link_stats(peer); }
    std::uint64_t edge_identity(PeerIndex peer) const override { return transport->edge_identity(peer); }
    void fail_repair(PeerIndex peer, unsigned code) override { transport->fail_repair(peer, code); }
    PeerIndex self() const override { return transport->self(); }
    std::uint32_t peer_count() const override { return transport->peer_count(); }

    core::Result<void> accept_offer() {
        if (transport->self() == 0 || phase != SetupPhase::reviewing_offer)
            return error<void>(SetupReason::options, "no client offer awaiting acceptance");
        if (auto built = initialize(); !built) return built;
        Bytes accept{setup_accept_kind};
        accept.insert(accept.end(), digest.begin(), digest.end());
        auto sent = transport->send(0, accept);
        if (!sent) return fail(SetupReason::link_closed, sent.error().message);
        phase = SetupPhase::barrier;
        return core::Result<void>::success();
    }

    bool needs_encoding(std::uint64_t tick, bool end) const override {
        return providers.monitor->needs_encoding(tick, end);
    }
    core::Result<void> record(std::uint64_t tick, std::string_view hash,
        std::span<const std::uint8_t> state, std::span<const std::uint8_t> input, bool end) override {
        auto result = providers.monitor->record(tick, hash, state, input, end);
        if (result && tick == 0) zero_recorded = true;
        return result;
    }
    core::Result<void> receive(const Received& received) override { return providers.monitor->receive(received); }
    core::Result<void> on_link_repair(PeerIndex peer, std::function<bool(PeerIndex)> ownership = {}) override {
        return providers.monitor->on_link_repair(peer, std::move(ownership));
    }
    core::Result<void> on_departure(PeerIndex peer, std::uint64_t tick) override {
        return providers.monitor->on_departure(peer, tick);
    }
    std::vector<PeerIndex> waiting_for_peers() const override { return providers.monitor->waiting_for_peers(); }
    bool frozen() const override { return refusal.has_value() || providers.monitor->frozen(); }
    core::Result<void> on_reconnect(std::uint64_t tick, std::string_view hash, std::span<const PeerIndex> inactive) override {
        return providers.monitor->on_reconnect(tick, hash, inactive);
    }
    bool ready_for_resync() const override { return providers.monitor->ready_for_resync(); }
    core::Result<void> on_resync(std::uint64_t tick, std::string_view hash) override { return providers.monitor->on_resync(tick, hash); }
    bool may_step() const override { return released && !refusal && providers.monitor->may_step(); }

    core::Result<void> refuse(SetupRefusal value, bool notify = true) {
        if (!refusal) {
            const auto normalize = [](std::string& text, std::size_t limit) {
                text = sanitize_lan_text(text, limit);
            };
            normalize(value.domain, 128); normalize(value.message, 1024);
            if (value.message.empty()) value.message = "setup refused";
            auto frame = encode_setup_refusal(value);
            if (!frame) {
                value.domain.clear(); value.message = "setup refused: diagnostic payload unavailable";
                value.local_hash.clear(); value.remote_hash.clear();
                frame = encode_setup_refusal(value);
            }
            refusal = std::move(value);
            phase = SetupPhase::refused;
            if (notify) {
                if (frame) for (PeerIndex peer = 0; peer < transport->peer_count(); ++peer)
                    if (peer != transport->self() && transport->link(peer) == LinkState::open)
                        static_cast<void>(transport->send(peer, frame.value()));
            }
        }
        return core::Result<void>::failure(refusal->diagnostic());
    }
    core::Result<void> fail(SetupReason reason, std::string message) {
        return refuse({reason, {}, std::move(message), {}, {}});
    }
    core::Result<void> initialize() {
        if (offer.seats.size() != transport->peer_count()) return fail(SetupReason::options, "offer roster differs from transport");
        if (offer.final_tick_count != options.session.final_tick_count)
            return fail(SetupReason::options, "local final tick count differs from agreed setup");
        auto local = providers.identify(offer);
        if (!local) return fail(SetupReason::local_build, local.error().message);
        if (auto valid = sim::validate_match_identity(local.value().content); !valid)
            return fail(SetupReason::local_build, valid.error().message);
        if (auto mismatch = sim::describe_mismatch(offer.identity, local.value())) {
            const std::string prefix = "match identity mismatch: ";
            const auto domain = offer.identity.build != local.value().build ? std::string("build")
                : (mismatch->message.starts_with(prefix) ? mismatch->message.substr(prefix.size()) : std::string("identity"));
            return refuse({SetupReason::identity, domain, mismatch->message, {}, {}});
        }
        auto built = providers.build(offer);
        if (!built) return fail(SetupReason::local_build, built.error().message);
        adapter = std::move(built).value();
        if (!adapter) return fail(SetupReason::local_build, "setup builder returned no adapter");
        std::vector<PeerSeat> seats;
        for (const auto& seat : offer.seats) seats.push_back({seat.seat, true});
        options.session.input_delay = offer.input_delay;
        options.session.final_tick_count = offer.final_tick_count;
        if (providers.configure_session) {
            auto configured = providers.configure_session(options.session, offer);
            if (!configured) return fail(SetupReason::local_build, configured.error().code + ": " + configured.error().message);
        }
        const bool recovery_enabled = offer.recovery.reconnect || offer.recovery.resync;
        if (options.session.reconnect.has_value() != recovery_enabled
            || options.session.resync.has_value() != offer.recovery.resync
            || (options.session.reconnect && options.session.reconnect->allow_rejoin != offer.recovery.reconnect))
            return fail(SetupReason::options, "local recovery providers differ from the agreed policy");
        if (recovery_enabled) {
            auto& recovery = *options.session.reconnect;
            recovery.grace_ms = offer.recovery.grace_ms; recovery.progress_timeout_ms = offer.recovery.progress_timeout_ms;
            recovery.hard_cap_ms = offer.recovery.hard_cap_ms; recovery.max_log_bytes = static_cast<std::size_t>(offer.recovery.max_log_bytes);
            if (options.session.resync) options.session.resync->timeout_ms = offer.recovery.resync_timeout_ms;
        }
        auto session = LockstepSession::create(*adapter, *this, std::move(seats), options.session, this);
        if (!session) return fail(SetupReason::local_build, session.error().message);
        core.emplace(std::move(session).value());
        // The core now owns the live credentials; setup retains policy only.
        if (options.session.reconnect) options.session.reconnect->seat_tokens.clear();
        core->set_message_handler([this](Received received) { return handle(std::move(received)); });
        accepted.assign(offer.seats.size(), false);
        accepted[transport->self()] = true;
        if (previewed.empty()) {
            previewed.assign(offer.seats.size(), false);
            previewed[transport->self()] = true;
        }
        return core::Result<void>::success();
    }
    core::Result<void> handle(Received received) {
        if (received.from >= transport->peer_count() || received.from == transport->self() || received.frame.empty())
            return fail(SetupReason::malformed, "invalid setup sender or empty frame");
        const auto& frame = received.frame;
        if (frame.front() == setup_refuse_kind) {
            auto value = decode_setup_refusal(frame);
            if (!value) return fail(SetupReason::malformed, value.error().message);
            return refuse(std::move(value).value(), transport->peer_count() > 2);
        }
        if (frame.front() == setup_offer_kind && transport->self() != 0 && received.from == 0
            && phase == SetupPhase::awaiting_offer) {
            auto decoded = decode_setup_offer(frame);
            if (!decoded) return fail(SetupReason::malformed, decoded.error().message);
            offer = std::move(decoded).value();
            std::copy(frame.end() - 32, frame.end(), digest.begin());
            phase = SetupPhase::reviewing_offer;
            Bytes preview{setup_preview_kind};
            preview.insert(preview.end(), digest.begin(), digest.end());
            if (auto sent = transport->send(0, preview); !sent) return fail(SetupReason::link_closed, sent.error().message);
            return options.auto_accept ? accept_offer() : core::Result<void>::success();
        }
        if (frame.front() == setup_accept_kind && transport->self() == 0 && core && frame.size() == 33
            && std::equal(digest.begin(), digest.end(), frame.begin() + 1)) {
            accepted[received.from] = true;
            previewed[received.from] = true;
            return core::Result<void>::success();
        }
        if (frame.front() == setup_preview_kind && transport->self() == 0 && core && frame.size() == 33
            && std::equal(digest.begin(), digest.end(), frame.begin() + 1)) {
            previewed[received.from] = true;
            return core::Result<void>::success();
        }
        if (frame.front() == setup_start_kind && transport->self() != 0 && received.from == 0 && core
            && frame.size() == 33 && std::equal(digest.begin(), digest.end(), frame.begin() + 1)) {
            started = true;
            return core::Result<void>::success();
        }
        if ((phase == SetupPhase::reviewing_offer || phase == SetupPhase::awaiting_offer) && (frame.front() == sealed_batch_kind
            || (frame.front() >= 32 && frame.front() <= 39))
            && frame.size() <= max_batch_bytes && held.size() < 8192 && held_bytes + frame.size() <= 8U * 1024U * 1024U) {
            held_bytes += frame.size();
            held.push_back(std::move(received));
            return core::Result<void>::success();
        }
        return fail(SetupReason::malformed, "unexpected setup message, phase or offer digest");
    }
};

namespace {
core::Result<std::shared_ptr<SetupRuntime>> runtime(MessageTransport& transport, SetupProviders providers, SetupOptions options) {
    if (transport.peer_count() < 2 || transport.peer_count() > max_match_seats || transport.self() >= transport.peer_count()
        || !providers.identify || !providers.build || !providers.monitor || !providers.tick_zero_mismatch
        || !providers.tick_zero_agreed || options.timeout_ms == 0 || options.session.max_frames_per_pump == 0)
        return error<std::shared_ptr<SetupRuntime>>(SetupReason::options, "invalid setup providers, timeout or transport roster");
    for (PeerIndex peer = 0; peer < transport.peer_count(); ++peer)
        if (peer != transport.self() && transport.link(peer) != LinkState::open)
            return error<std::shared_ptr<SetupRuntime>>(SetupReason::options, "setup requires open peer streams");
    auto value = std::make_shared<SetupRuntime>();
    value->transport = &transport; value->providers = std::move(providers); value->options = options;
    return core::Result<std::shared_ptr<SetupRuntime>>::success(std::move(value));
}
}
core::Result<SetupSession> SetupSession::host(MessageTransport& transport, SetupOffer offer,
    SetupProviders providers, SetupOptions options) {
    if (transport.self() != 0) return error<SetupSession>(SetupReason::options, "only peer zero may offer setup");
    auto frame = encode_setup_offer(offer);
    if (!frame) return core::Result<SetupSession>::failure(frame.error());
    auto created = runtime(transport, std::move(providers), options);
    if (!created) return core::Result<SetupSession>::failure(created.error());
    auto value = std::move(created).value();
    value->offer = std::move(offer);
    std::copy(frame.value().end() - 32, frame.value().end(), value->digest.begin());
    if (auto built = value->initialize(); !built) return core::Result<SetupSession>::failure(built.error());
    value->phase = SetupPhase::awaiting_accept;
    value->start_requested = options.auto_start;
    for (PeerIndex peer = 1; peer < transport.peer_count(); ++peer) {
        auto sent = transport.send(peer, frame.value());
        if (!sent) return core::Result<SetupSession>::failure(value->fail(SetupReason::link_closed, sent.error().message).error());
    }
    return core::Result<SetupSession>::success(SetupSession(std::move(value)));
}
core::Result<SetupSession> SetupSession::join(MessageTransport& transport, SetupProviders providers, SetupOptions options) {
    if (transport.self() == 0) return error<SetupSession>(SetupReason::options, "host cannot join itself");
    auto created = runtime(transport, std::move(providers), options);
    if (!created) return core::Result<SetupSession>::failure(created.error());
    return core::Result<SetupSession>::success(SetupSession(std::move(created).value()));
}
core::Result<void> SetupSession::pump(std::uint64_t now) {
    auto& value = *runtime_;
    if (value.refusal) return core::Result<void>::failure(value.refusal->diagnostic());
    if (value.taken) return error<void>(SetupReason::options, "setup result already transferred");
    if (value.last_now && now < *value.last_now) return value.fail(SetupReason::options, "setup clock moved backwards");
    value.last_now = now;
    for (PeerIndex peer = 0; peer < value.transport->peer_count(); ++peer)
        if (peer != value.transport->self() && value.transport->link(peer) == LinkState::closed)
            return value.fail(SetupReason::link_closed, "peer disconnected during setup");
    if (!value.core) {
        for (std::size_t i = 0; i < value.options.session.max_frames_per_pump && !value.core; ++i) {
            auto received = value.transport->poll();
            if (!received) break;
            if (auto handled = value.handle(std::move(*received)); !handled) return handled;
        }
    }
    if (value.core) {
        auto pumped = value.core->pump(now); // Forward zero/receive, gate every step.
        if (value.refusal) return core::Result<void>::failure(value.refusal->diagnostic());
        if (auto mismatch = value.providers.tick_zero_mismatch())
            return value.refuse({SetupReason::tick_zero, mismatch->channel, "tick-zero state differs",
                mismatch->local_hash, mismatch->remote_hash});
        if (!pumped) return value.fail(SetupReason::local_build, pumped.error().message);
        const bool all = value.transport->self() != 0
            || std::all_of(value.accepted.begin(), value.accepted.end(), [](bool accepted) { return accepted; });
        if (all) value.phase = SetupPhase::barrier;
        if (all && value.transport->self() == 0 && value.start_requested && !value.started
            && (!value.providers.credentials_delivered || value.providers.credentials_delivered())) {
            Bytes frame{setup_start_kind};
            frame.insert(frame.end(), value.digest.begin(), value.digest.end());
            for (PeerIndex peer = 1; peer < value.transport->peer_count(); ++peer)
                if (auto sent = value.transport->send(peer, frame); !sent)
                    return value.fail(SetupReason::link_closed, sent.error().message);
            value.started = true;
        }
        if (all && value.started && value.zero_recorded && value.providers.tick_zero_agreed()) value.phase = SetupPhase::ready;
    }
    // Preview/accept/host Start are human-paced. Only offer delivery and the
    // started tick-zero exchange have deadlines; transport detects dead peers.
    const bool offer_pending = value.phase == SetupPhase::awaiting_offer || (value.transport->self() == 0
        && !std::all_of(value.previewed.begin(), value.previewed.end(), [](bool seen) { return seen; }));
    const unsigned timed_phase = offer_pending ? 1U : (value.started && value.phase != SetupPhase::ready ? 2U : 0U);
    if (timed_phase != value.timed_phase || !value.since) value.since = now;
    value.timed_phase = timed_phase;
    if (timed_phase != 0 && now - *value.since >= value.options.timeout_ms)
        return value.fail(SetupReason::timeout, "setup readiness wait expired");
    return core::Result<void>::success();
}
core::Result<void> SetupSession::receive_offer(Received received) {
    if (runtime_->phase != SetupPhase::awaiting_offer || received.frame.empty()
        || received.frame.front() != setup_offer_kind)
        return error<void>(SetupReason::options, "no buffered host offer expected in this phase");
    return runtime_->handle(std::move(received));
}
core::Result<SetupResult> SetupSession::take_result() {
    if (runtime_->refusal) return core::Result<SetupResult>::failure(runtime_->refusal->diagnostic());
    if (runtime_->phase != SetupPhase::ready || runtime_->taken)
        return error<SetupResult>(SetupReason::options, "setup barrier has not completed or result already transferred");
    runtime_->taken = true; runtime_->released = true;
    return core::Result<SetupResult>::success(SetupResult(runtime_));
}
core::Result<void> SetupSession::cancel() {
    if (runtime_->taken) return error<void>(SetupReason::options, "setup ownership already transferred to the session");
    return runtime_->fail(SetupReason::cancelled, "setup cancelled");
}
core::Result<void> SetupSession::accept() { return runtime_->accept_offer(); }
void SetupSession::erase_reconnect_credentials() noexcept {
    if (runtime_->options.session.reconnect) runtime_->options.session.reconnect->seat_tokens.clear();
    if (runtime_->core) runtime_->core->erase_reconnect_credentials();
}
core::Result<void> SetupSession::start() {
    if (runtime_->transport->self() != 0 || runtime_->taken || runtime_->refusal)
        return error<void>(SetupReason::options, "only an active setup host may request start");
    runtime_->start_requested = true;
    return core::Result<void>::success();
}
const SetupOffer* SetupSession::offered() const {
    return runtime_->phase == SetupPhase::awaiting_offer ? nullptr : &runtime_->offer;
}
bool SetupSession::start_received() const { return runtime_->started; }
SetupPhase SetupSession::phase() const { return runtime_->phase; }
const std::optional<SetupRefusal>& SetupSession::refusal() const { return runtime_->refusal; }
const SetupOffer& SetupResult::offer() const { return runtime_->offer; }
SessionAdapter& SetupResult::adapter() { return *runtime_->adapter; }
LockstepSession& SetupResult::session() { return *runtime_->core; }
const std::optional<SetupRefusal>& SetupResult::refusal() const { return runtime_->refusal; }

} // namespace eawr::net
