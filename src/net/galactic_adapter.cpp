#include "eawr/net/galactic_adapter.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/tactical/admission.hpp"

#include <algorithm>
#include <type_traits>
#include <utility>
#include <variant>

namespace eawr::net {
namespace {
namespace galactic = sim::galactic;
using Bytes = std::vector<std::uint8_t>;

template<class T> core::Result<T> fail_with(std::string_view code, std::string message) {
    core::Diagnostic error;
    error.code = code;
    error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}

// Deliberately no default: a new payload must receive an explicit classification.
template <typename Payload> struct PayloadPolicy;
#define EAWR_GALACTIC_POLICY(Payload, Authority) \
    template <> struct PayloadPolicy<galactic::Payload> { \
        static constexpr auto authority = GalacticPayloadAuthority::Authority; }
EAWR_GALACTIC_POLICY(PauseGalaxy, human_wire);
EAWR_GALACTIC_POLICY(ResumeGalaxy, human_wire);
EAWR_GALACTIC_POLICY(SetSpeed, human_wire);
EAWR_GALACTIC_POLICY(DebugSetPlanetOwner, human_wire); // only when the agreed setup enables debug commands
EAWR_GALACTIC_POLICY(DebugSetFleetOrbit, human_wire);
EAWR_GALACTIC_POLICY(TacticalLayerCommand, human_wire);
EAWR_GALACTIC_POLICY(SaveRequest, human_wire);
EAWR_GALACTIC_POLICY(PostLoadFixup, engine_only); // internal restore prologue
EAWR_GALACTIC_POLICY(CreateFleet, human_wire);
EAWR_GALACTIC_POLICY(AddFleetMembers, human_wire);
EAWR_GALACTIC_POLICY(RemoveFleetMembers, human_wire);
EAWR_GALACTIC_POLICY(MergeFleets, human_wire);
EAWR_GALACTIC_POLICY(SetFleetOrbitSlot, human_wire);
EAWR_GALACTIC_POLICY(ProductionCommand, human_wire);
EAWR_GALACTIC_POLICY(SellObject, human_wire);
EAWR_GALACTIC_POLICY(SeatLeave, human_wire); // a seat leaves itself; the session service synthesizes the rest
EAWR_GALACTIC_POLICY(SeatRejoin, engine_only); // trusted session service only (held-seat rejoin is GCMP-15)
EAWR_GALACTIC_POLICY(AssignBattleTeamRole, human_wire);
EAWR_GALACTIC_POLICY(RecheckOrbit, engine_only); // script-only dispatch (WGM-154)
EAWR_GALACTIC_POLICY(MoveFleet, human_wire);
EAWR_GALACTIC_POLICY(LandFleet, human_wire);
EAWR_GALACTIC_POLICY(BattleChoice, human_wire);
EAWR_GALACTIC_POLICY(AutoResolveWithdraw, human_wire);
EAWR_GALACTIC_POLICY(ObserverCommand, engine_only); // ability, control and clock owners via the script seat
EAWR_GALACTIC_POLICY(PrepareBlackMarket, human_wire);
EAWR_GALACTIC_POLICY(BuyBlackMarket, human_wire);
EAWR_GALACTIC_POLICY(PrepareGalacticAbility, human_wire);
EAWR_GALACTIC_POLICY(DeployGroundAbility, human_wire);
EAWR_GALACTIC_POLICY(ExecutePreparedAbility, human_wire);
EAWR_GALACTIC_POLICY(CancelGalacticAbility, human_wire);
EAWR_GALACTIC_POLICY(LaunchFromSurface, human_wire);
EAWR_GALACTIC_POLICY(LaunchFleet, human_wire);
EAWR_GALACTIC_POLICY(TransferSurface, human_wire);
EAWR_GALACTIC_POLICY(TransferFleetMembers, human_wire);
EAWR_GALACTIC_POLICY(SetFleetPathfinder, human_wire);
EAWR_GALACTIC_POLICY(BattleSummaryAcknowledge, human_wire);
EAWR_GALACTIC_POLICY(InvasionAccept, human_wire); // the sending seat accepts its own prepared selection (GCMP-04)
#undef EAWR_GALACTIC_POLICY

void put(Bytes& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index) bytes.push_back(static_cast<std::uint8_t>(value >> (index * 8U)));
}

constexpr std::size_t header_bytes = 3;
// A sealed command costs 16 bytes of envelope inside a batch of at most max_batch_bytes.
constexpr std::size_t max_input_bytes = max_batch_bytes - 41;
} // namespace

GalacticPayloadAuthority galactic_payload_authority(const galactic::CommandPayload& payload) noexcept {
    return std::visit([](const auto& value) {
        return PayloadPolicy<std::remove_cvref_t<decltype(value)>>::authority;
    }, payload);
}

core::Result<Bytes> encode_galactic_input(const sim::SeatCommandKey key, const galactic::CommandPayload& payload) {
    if (key.tick >= galactic::galactic_max_ticks || key.seat.value == 0)
        return fail_with<Bytes>(session_errors::malformed, "galactic input stamp is outside the session limits");
    const auto body = galactic::encode_payload(payload);
    const auto layer = galactic::layer_of(payload);
    const auto opcode = galactic::opcode_of(payload);
    if (galactic::decode_payload(layer, opcode, body) != std::optional(payload))
        return fail_with<Bytes>(session_errors::malformed, "galactic payload does not round-trip");
    if (body.size() > max_input_bytes - header_bytes)
        return fail_with<Bytes>(session_errors::resource_limit, "galactic input exceeds frame byte budget");
    Bytes bytes;
    bytes.reserve(header_bytes + body.size());
    bytes.push_back(static_cast<std::uint8_t>(layer));
    put(bytes, opcode, 2);
    bytes.insert(bytes.end(), body.begin(), body.end());
    return core::Result<Bytes>::success(std::move(bytes));
}

core::Result<galactic::GameCommand> decode_galactic_input(const SeatInput& input) {
    using Result = core::Result<galactic::GameCommand>;
    if (input.key.tick >= galactic::galactic_max_ticks || input.key.seat.value == 0
        || input.bytes.size() < header_bytes || input.bytes.size() > max_input_bytes)
        return fail_with<galactic::GameCommand>(session_errors::malformed, "invalid galactic input size or stamp");
    const auto layer_byte = input.bytes[0];
    if (layer_byte < static_cast<std::uint8_t>(galactic::CommandLayer::session)
        || layer_byte > static_cast<std::uint8_t>(galactic::CommandLayer::tactical))
        return fail_with<galactic::GameCommand>(session_errors::malformed, "unknown galactic command layer");
    const auto layer = static_cast<galactic::CommandLayer>(layer_byte);
    const auto opcode = static_cast<std::uint16_t>(input.bytes[1] | (input.bytes[2] << 8U));
    const Bytes body(input.bytes.begin() + static_cast<std::ptrdiff_t>(header_bytes), input.bytes.end());
    auto payload = galactic::decode_payload(layer, opcode, body);
    if (!payload) return fail_with<galactic::GameCommand>(session_errors::malformed, "unknown or malformed galactic payload");
    if (galactic::layer_of(*payload) != layer || galactic::opcode_of(*payload) != opcode
        || galactic::encode_payload(*payload) != body)
        return fail_with<galactic::GameCommand>(session_errors::malformed, "noncanonical galactic command encoding");
    return Result::success({{input.key.tick, input.key.seat, input.key.sequence}, std::move(*payload)});
}

GalacticSessionAdapter::GalacticSessionAdapter(campaign::GameSession& game, const sim::PartitionExecutor& executor,
    GalacticAdapterOptions options)
    : game_(&game), executor_(&executor), options_(options), clock_limiter_(options.clock_policy),
      roster_(game.galaxy().setup().seats), debug_commands_(game.galaxy().setup().debug_commands) {
    std::sort(roster_.begin(), roster_.end(), [](const auto& left, const auto& right) { return left.seat_id < right.seat_id; });
}

std::uint64_t GalacticSessionAdapter::completed_tick() const { return game_->completed_tick(); }
std::string GalacticSessionAdapter::state_hash() const { return clock_hash(game_->state_sha256()); }

Bytes GalacticSessionAdapter::clock_state() const {
    Bytes bytes;
    put(bytes, static_cast<std::uint8_t>(clock_limiter_.policy()), 1);
    put(bytes, GalacticClockLimiter::window_ticks, 8);
    put(bytes, GalacticClockLimiter::changes_per_window, 4);
    put(bytes, pause_actor_.has_value() ? 1U : 0U, 1);
    put(bytes, pause_actor_ ? pause_actor_->value : 0U, 4);
    put(bytes, clock_limiter_.history().size(), 4);
    for (const auto& [seat_id, changes] : clock_limiter_.history()) {
        put(bytes, seat_id.value, 4);
        put(bytes, changes.size(), 4);
        for (const auto tick : changes) put(bytes, tick, 8);
    }
    put(bytes, clock_limiter_.toggles().size(), 4);
    for (const auto& [seat_id, tick] : clock_limiter_.toggles()) { put(bytes, seat_id.value, 4); put(bytes, tick, 8); }
    return bytes;
}

std::string GalacticSessionAdapter::clock_hash(std::string base) const {
    if (options_.clock_policy == GalacticClockPolicy::unrestricted) return base;
    constexpr std::string_view domain = "eawr-galactic-lan-clock-v2";
    Bytes bytes(domain.begin(), domain.end());
    bytes.insert(bytes.end(), base.begin(), base.end());
    const auto clock = clock_state();
    bytes.insert(bytes.end(), clock.begin(), clock.end());
    return core::sha256_hex(bytes);
}

Bytes GalacticSessionAdapter::canonical_state() const {
    auto bytes = game_->galaxy().canonical_state_bytes();
    // While a child battle runs the whole-session state is the galaxy plus the child (ADR-012 section 3).
    if (const auto* child = game_->battle()) {
        const auto tactical = child->canonical_state_bytes();
        bytes.push_back(1);
        put(bytes, game_->conflict() ? game_->conflict()->id : 0, 8);
        put(bytes, tactical.size(), 8);
        bytes.insert(bytes.end(), tactical.begin(), tactical.end());
    }
    if (options_.clock_policy != GalacticClockPolicy::unrestricted) {
        const auto clock = clock_state();
        bytes.push_back(0x43); // LAN clock extension, independent of campaign checkpoint bytes.
        const auto base = game_->state_sha256();
        bytes.insert(bytes.end(), base.begin(), base.end());
        put(bytes, clock.size(), 8);
        bytes.insert(bytes.end(), clock.begin(), clock.end());
    }
    return bytes;
}

const galactic::Seat* GalacticSessionAdapter::seat(const sim::SeatId id) const {
    const auto found = std::lower_bound(roster_.begin(), roster_.end(), id,
        [](const auto& value, const auto key) { return value.seat_id < key; });
    return found == roster_.end() || found->seat_id != id ? nullptr : &*found;
}

core::Result<galactic::GameCommand> GalacticSessionAdapter::authorize(const SeatInput& input) const {
    auto decoded = decode_galactic_input(input);
    if (!decoded) return decoded;
    // Only static facts: the agreed tick-zero roster, the payload class and the agreed setup flags. Anything that
    // depends on live state (an alert, a departure) is decided at execution, identically on every peer.
    const auto* issuer = seat(input.key.seat);
    if (!issuer) return fail_with<galactic::GameCommand>(session_errors::admission, "galactic input has unbound seat");
    if (issuer->kind != sim::SeatKind::human || issuer->player == 0)
        return fail_with<galactic::GameCommand>(session_errors::admission, "only commandable human seats may send galactic input");
    const auto& payload = decoded.value().payload;
    // D8: a Galactic MP seat stays departed until checkpoint-based rejoin is available (GCMP-15).
    if (std::holds_alternative<galactic::SeatRejoin>(payload))
        return fail_with<galactic::GameCommand>(session_errors::admission,
            "Galactic multiplayer rejoin is disabled; a departed seat cannot return (GCMP-15, #4368).");
    if (galactic_payload_authority(payload) != GalacticPayloadAuthority::human_wire)
        return fail_with<galactic::GameCommand>(session_errors::admission, "engine-only payload cannot cross the human wire");
    if (!debug_commands_ && (std::holds_alternative<galactic::DebugSetPlanetOwner>(payload)
        || std::holds_alternative<galactic::DebugSetFleetOrbit>(payload)))
        return fail_with<galactic::GameCommand>(session_errors::admission, "debug commands are off in this session");
    if (!options_.child_battle_commands && std::holds_alternative<galactic::TacticalLayerCommand>(payload))
        return fail_with<galactic::GameCommand>(session_errors::admission,
            "child battle commands are disabled for this driver");
    return decoded;
}

core::Result<void> GalacticSessionAdapter::validate(const SeatInput& input) const {
    auto command = authorize(input);
    if (!command) return core::Result<void>::failure(command.error());
    return core::Result<void>::success();
}

bool GalacticSessionAdapter::is_departure(const SeatInput& input) const {
    const auto command = authorize(input);
    return command && std::holds_alternative<galactic::SeatLeave>(command.value().payload);
}

core::Result<void> GalacticSessionAdapter::install(const std::span<const SeatInput> canonical) {
    const auto tick = game_->completed_tick();
    std::vector<galactic::GameCommand> decoded;
    decoded.reserve(canonical.size());
    for (std::size_t index = 0; index < canonical.size(); ++index) {
        const auto& input = canonical[index];
        // Only a complete, sealed batch for exactly the tick about to execute reaches the owner.
        if (input.key.tick != tick)
            return fail_with<void>(session_errors::admission, "installed input is not stamped for the executing tick");
        if (index && !(canonical[index - 1].key < input.key))
            return fail_with<void>(session_errors::malformed, "installed input is not in canonical order");
        auto command = authorize(input);
        if (!command) return core::Result<void>::failure(command.error());
        // Without WGS-P01..P04 the simulation rejects SeatLeave, while the network would remove its voter.
        // Refuse before submitting anything rather than publishing a departure that never changed control.
        if (std::holds_alternative<galactic::SeatLeave>(command.value().payload)
            && (options_.departure_policy != GalacticDeparturePolicy::configured_fallback || !game_->galaxy().seat_control()))
            return fail_with<void>(session_errors::admission,
                "Galactic multiplayer departure requires a configured seat-control fallback (WGS-P04).");
        decoded.push_back(std::move(command).value());
    }
    tick_commands_.clear();
    tick_refusal_begin_ = refused_.size();
    clock_limiter_.expire(tick);
    clock_refusals_.clear();
    bool predicted_pause = game_->battle() ? game_->child_paused()
        : game_->galaxy().clock().pause_depth > (game_->pending_conflict() || game_->battle_summary_hold() ? 1U : 0U);
    for (auto& command : decoded) {
        const bool pause_request = std::holds_alternative<galactic::PauseGalaxy>(command.payload);
        const bool resume_request = std::holds_alternative<galactic::ResumeGalaxy>(command.payload);
        const bool speed_request = std::holds_alternative<galactic::SetSpeed>(command.payload);
        const auto live_seats = game_->galaxy().seats();
        const auto issuer = std::find_if(live_seats.begin(), live_seats.end(),
            [&](const auto& row) { return row.seat_id == command.key.seat; });
        const bool live_issuer = issuer != live_seats.end() && issuer->active;
        // #4574: pause and resume never spend the speed budget. Resume is always allowed; a pause only has the debounce.
        if (live_issuer && resume_request && predicted_pause) clock_limiter_.note_resume(command.key.seat, tick);
        const bool debounced = live_issuer && pause_request && !predicted_pause
            && !clock_limiter_.admit_pause(command.key.seat, tick);
        const bool speed_limited = live_issuer && speed_request && !clock_limiter_.admit_speed(command.key.seat, tick);
        if (debounced || speed_limited) {
            std::string text = "Pause repeated too quickly; try again in a second.";
            if (speed_limited) {
                if (std::find(clock_refusals_.begin(), clock_refusals_.end(), command.key.seat) == clock_refusals_.end())
                    clock_refusals_.push_back(command.key.seat);
                text = "Shared clock limit: speed changes are limited to " + std::to_string(ClockThrottle::speed_changes_per_window)
                    + " per " + std::to_string(ClockThrottle::window_seconds) + " session seconds per seat; the next is allowed in "
                    + std::to_string(clock_limiter_.speed_wait_seconds(command.key.seat, tick)) + " s.";
            }
            refused_.push_back(fail_with<void>(session_errors::admission, text).error());
            tick_commands_.push_back(std::move(command)); // Attempts remain in the agreed input journal.
            continue;
        }
        if (issuer != live_seats.end() && issuer->active && issuer->kind == galactic::SeatKind::human) {
            if (pause_request && !predicted_pause) pause_actor_ = command.key.seat;
            if (pause_request) predicted_pause = true;
            if (resume_request) predicted_pause = false;
        }
        if (const auto* nested = std::get_if<galactic::TacticalLayerCommand>(&command.payload);
            nested && game_->battle() && game_->battle()->controller_policy()) {
            namespace tactical = sim::tactical;
            // Only the sealed executing tick may consult the canonical child.
            // validate() above depends exclusively on the agreed static roster.
            const auto& setup = game_->battle()->initial_setup();
            const auto view = game_->battle_view(command.key.seat);
            auto inner = tactical::decode_command_body(nested->body, setup,
                view ? view.value().perspective : 0U);
            const bool reserve = inner && std::holds_alternative<tactical::ReinforcePayload>(inner.value().payload);
            if (!inner || tactical::command_body_opcode(inner.value()) != nested->opcode
                || inner.value().origin == tactical::CommandOrigin::engine
                || (inner.value().issuing_seat.value != 0 && inner.value().issuing_seat != command.key.seat)
                || (!reserve && !tactical::human_wire_payload(inner.value().payload))) {
                refused_.push_back(fail_with<void>(session_errors::admission,
                    "forged or engine-only tactical route cannot cross the human wire").error());
                tick_commands_.push_back(std::move(command));
                continue;
            }
            // Reserve availability and per-source control are rechecked by the child.
        }
        if (auto submitted = game_->submit(command); !submitted) {
            // Installation is the executing sealed tick. An input that outlived its conflict is a shared
            // gameplay refusal, not a failed network session; retain it in the canonical input and report it.
            if (std::holds_alternative<galactic::BattleChoice>(command.payload)
                || (options_.child_battle_commands && std::holds_alternative<galactic::TacticalLayerCommand>(command.payload))) {
                refused_.push_back(submitted.error());
                tick_commands_.push_back(std::move(command));
                continue;
            }
            return fail_with<void>(session_errors::simulation, submitted.error().message);
        }
        tick_commands_.push_back(std::move(command));
    }
    return core::Result<void>::success();
}

core::Result<CompletedTick> GalacticSessionAdapter::step() {
    const auto tick = game_->completed_tick();
    if (engine_source_) {
        auto produced = engine_source_(*game_, tick);
        if (!produced) return core::Result<CompletedTick>::failure(produced.error());
        for (auto& command : produced.value()) {
            const auto* issuer = seat(command.key.seat);
            if (command.key.tick != tick || !issuer || issuer->kind == sim::SeatKind::human)
                return fail_with<CompletedTick>(session_errors::admission,
                    "engine source must issue for the executing tick from an AI or script seat");
            if (auto submitted = game_->submit(command); !submitted)
                return fail_with<CompletedTick>(session_errors::simulation, submitted.error().message);
            tick_commands_.push_back(std::move(command));
        }
    }
    auto stepped = game_->step(*executor_);
    if (!stepped) return fail_with<CompletedTick>(session_errors::simulation, stepped.error().message);
    last_input_.clear();
    std::sort(tick_commands_.begin(), tick_commands_.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    put(last_input_, tick_commands_.size(), 4); // uniform encoding: an empty tick is count 0
    for (const auto& command : tick_commands_) {
        auto bytes = encode_galactic_input({command.key.tick, command.key.seat, command.key.sequence}, command.payload);
        if (!bytes) return core::Result<CompletedTick>::failure(bytes.error());
        put(last_input_, command.key.tick, 8);
        put(last_input_, command.key.seat.value, 4);
        put(last_input_, command.key.sequence, 8);
        put(last_input_, bytes.value().size(), 4);
        last_input_.insert(last_input_.end(), bytes.value().begin(), bytes.value().end());
    }
    tick_commands_.clear();
    last_tick_ = std::move(stepped).value();
    last_tick_->diagnostics.insert(last_tick_->diagnostics.end(),
        refused_.begin() + static_cast<std::ptrdiff_t>(tick_refusal_begin_), refused_.end());
    const auto base_hash = last_tick_->state_sha256.empty() ? last_tick_->state_hash.get() : last_tick_->state_sha256;
    const auto hash = clock_hash(base_hash);
    last_tick_->state_sha256 = hash;
    last_tick_->state_hash = sim::StateHash(hash);
    return core::Result<CompletedTick>::success({last_tick_->completed_tick, hash});
}

core::Result<Bytes> GalacticSessionAdapter::canonical_input() const {
    return core::Result<Bytes>::success(last_input_);
}

} // namespace eawr::net
