#pragma once

#include "eawr/net/lockstep_session.hpp"
#include "eawr/sim/match_identity.hpp"
#include "eawr/sim/tactical/replay.hpp"

#include <memory>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace eawr::net {

// Coordinator-reserved LAN-3 message kinds, in the 40..47 range.
inline constexpr std::uint8_t setup_offer_kind = 40, setup_accept_kind = 41, setup_refuse_kind = 42;
inline constexpr std::uint8_t setup_start_kind = 43;
inline constexpr std::uint8_t setup_preview_kind = 44;
inline constexpr std::size_t max_setup_bytes = 1024U * 1024U;
inline constexpr std::string_view tactical_setup_domain = "tactical-skirmish-v1";

struct SetupSeat {
    PeerIndex peer{};
    sim::SeatId seat{};
    sim::tactical::PlayerId player{};
    friend bool operator==(const SetupSeat&, const SetupSeat&) = default;
};

struct SetupRecoveryPolicy {
    bool reconnect{}, resync{};
    std::uint64_t grace_ms{30'000}, progress_timeout_ms{30'000}, hard_cap_ms{300'000};
    std::uint64_t max_log_bytes{64U * 1024U * 1024U}, resync_timeout_ms{30'000};
    friend bool operator==(const SetupRecoveryPolicy&, const SetupRecoveryPolicy&) = default;
};

struct SetupOffer {
    std::uint64_t seed{};
    std::string domain{tactical_setup_domain};
    std::vector<std::uint8_t> setup; // Canonical command-free replay, including SKSU.
    std::vector<SetupSeat> seats; // Strict peer-index order; AI slots live in SKSU.
    std::uint64_t input_delay{3};
    sim::MatchAgreement identity;
    SetupRecoveryPolicy recovery;
    std::uint64_t final_tick_count{max_session_ticks}; // Agreed simulation limit; real matches use the default.
};

enum class SetupReason : std::uint8_t {
    malformed = 0, options = 1, identity = 2, tick_zero = 3,
    local_build = 4, link_closed = 5, timeout = 6, cancelled = 7
};

struct SetupRefusal {
    SetupReason reason{};
    std::string domain;
    std::string message;
    std::string local_hash, remote_hash; // Set for the tick-zero refusal, unchanged on receipt.
    [[nodiscard]] core::Diagnostic diagnostic() const;
    friend bool operator==(const SetupRefusal&, const SetupRefusal&) = default;
};

// Offer v3: kind:u8, version:u32, seed:u64, delay:u64, final tick count:u64, domain:text,
// setup:blob, seat count:u32 and (peer,seat,player):u32 triples, build:text,
// canonical MPS-6 identity:blob + combined digest:32, recovery flags:u8 and five limits:u64, offer SHA-256:32.
// Text/blob lengths are LE u32; every integer is LE. Accept is kind + offer
// digest:32; Start and Preview use the same envelope. Preview acknowledges delivery
// so human review/accept/Start waits have no network deadline. Refusal is kind, reason:u8, then
// domain/message/local/remote:text.
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_setup_offer(const SetupOffer& offer);
[[nodiscard]] core::Result<SetupOffer> decode_setup_offer(std::span<const std::uint8_t> frame);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_setup_refusal(const SetupRefusal& refusal);
[[nodiscard]] core::Result<SetupRefusal> decode_setup_refusal(std::span<const std::uint8_t> frame);
[[nodiscard]] core::Result<std::vector<std::uint8_t>> encode_skirmish_setup(const sim::tactical::TacticalSetup& setup);
[[nodiscard]] core::Result<sim::tactical::TacticalSetup> decode_skirmish_setup(std::span<const std::uint8_t> bytes);

struct TickZeroMismatch {
    std::string local_hash, remote_hash, channel;
    TickZeroMismatch(std::string local, std::string remote, std::string source)
        : local_hash(std::move(local)), remote_hash(std::move(remote)), channel(std::move(source)) {}
};
struct SetupProviders {
    // Compute from local installed content, never from the host's digests. The offer
    // selects the map/options; the ordinary skirmish builder supplies local content.
    std::function<core::Result<sim::MatchAgreement>(const SetupOffer&)> identify;
    std::function<core::Result<std::unique_ptr<SessionAdapter>>(const SetupOffer&)> build;
    // Required LAN-4 monitor; outlives the ready result. Caller must configure it
    // with SetupOffer::input_delay and final_tick_count where relevant.
    SessionMonitor* monitor{};
    // Local providers attach factories/credentials after verifying the offer. Secrets never enter the offer.
    std::function<core::Result<void>(SessionOptions&, const SetupOffer&)> configure_session;
    std::function<bool()> tick_zero_agreed; // LAN-4 DesyncMonitor::setup_agreed().
    std::function<std::optional<TickZeroMismatch>()> tick_zero_mismatch;
    std::function<bool()> credentials_delivered; // Host Start waits for every delivery acknowledgement.
};

struct SetupOptions {
    SessionOptions session{}; // D comes from the offer; final_tick_count must equal the offered value.
    std::uint64_t timeout_ms{30'000};
    bool auto_accept{true}; // UI callers disable this to show the offer before accepting.
    bool auto_start{true}; // UI hosts disable this until the user presses Start.
};
enum class SetupPhase { awaiting_offer, reviewing_offer, awaiting_accept, barrier, ready, refused };
struct SetupRuntime;

// Owns the locally built adapter and core with stable addresses. Transport,
// executor (owned by the adapter provider) and monitor must outlive this result.
class SetupResult final {
public:
    [[nodiscard]] const SetupOffer& offer() const;
    [[nodiscard]] SessionAdapter& adapter();
    [[nodiscard]] LockstepSession& session();
    [[nodiscard]] const std::optional<SetupRefusal>& refusal() const;
private:
    friend class SetupSession;
    explicit SetupResult(std::shared_ptr<SetupRuntime> runtime) : runtime_(std::move(runtime)) {}
    std::shared_ptr<SetupRuntime> runtime_;
};

// Single-threaded, caller-time-driven setup over already open streams. LAN-1
// records tick zero through LAN-4; a forwarding gate holds all stepping until
// take_result(). No separate hash protocol or transport polling thread.
class SetupSession final {
public:
    [[nodiscard]] static core::Result<SetupSession> host(MessageTransport& transport,
        SetupOffer offer, SetupProviders providers, SetupOptions options = {});
    [[nodiscard]] static core::Result<SetupSession> join(MessageTransport& transport,
        SetupProviders providers, SetupOptions options = {});
    [[nodiscard]] core::Result<void> pump(std::uint64_t now_ms);
    // Replay the host offer buffered by the UI before handing transport ownership
    // to the simulation thread; identical validation to a polled offer. Buffer
    // exactly this one frame here; the caller must not poll subsequent frames.
    [[nodiscard]] core::Result<void> receive_offer(Received received);
    [[nodiscard]] core::Result<SetupResult> take_result();
    [[nodiscard]] core::Result<void> cancel();
    [[nodiscard]] core::Result<void> accept();
    [[nodiscard]] core::Result<void> start(); // Host request; sent after every seat accepts.
    void erase_reconnect_credentials() noexcept; // Terminal owner, including a failed barrier.
    [[nodiscard]] const SetupOffer* offered() const;
    [[nodiscard]] bool start_received() const;
    [[nodiscard]] SetupPhase phase() const;
    [[nodiscard]] const std::optional<SetupRefusal>& refusal() const;
private:
    explicit SetupSession(std::shared_ptr<SetupRuntime> runtime) : runtime_(std::move(runtime)) {}
    std::shared_ptr<SetupRuntime> runtime_;
};

} // namespace eawr::net
