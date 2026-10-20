#include "eawr/net/galactic_adapter.hpp"

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
EAWR_GALACTIC_POLICY(TacticalLayerCommand, human_wire); // only once the child barrier is enabled (GCMP-05)
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
EAWR_GALACTIC_POLICY(ExecutePreparedAbility, human_wire);
EAWR_GALACTIC_POLICY(CancelGalacticAbility, human_wire);
EAWR_GALACTIC_POLICY(LaunchFromSurface, human_wire);
EAWR_GALACTIC_POLICY(LaunchFleet, human_wire);
EAWR_GALACTIC_POLICY(TransferFleetMembers, human_wire);
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
    : game_(&game), executor_(&executor), options_(options),
      roster_(game.galaxy().setup().seats), debug_commands_(game.galaxy().setup().debug_commands) {
    std::sort(roster_.begin(), roster_.end(), [](const auto& left, const auto& right) { return left.seat_id < right.seat_id; });
}

std::uint64_t GalacticSessionAdapter::completed_tick() const { return game_->completed_tick(); }
std::string GalacticSessionAdapter::state_hash() const { return game_->state_sha256(); }

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
            "child battle commands are disabled until the Begin/return barrier lands (#1115/#2016)");
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
    for (auto& command : decoded) {
        if (auto submitted = game_->submit(command); !submitted) {
            // WGM-174/177: GameSession::submit refuses a choice for an alert that already closed, reading its
            // pending_alert view (local seat, wall-clock elapsed, local votes). That view is NOT agreed state, so
            // this refusal is not peer-deterministic once a reader is wired: it is only safe while none is (no
            // setter exists on main). The agreed path is GCMP-04 (#4360), which decides a BattleChoice at execution
            // from shared state; until then the refusal is recorded and the match continues.
            if (std::holds_alternative<galactic::BattleChoice>(command.payload)) {
                refused_.push_back(submitted.error());
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
    const auto hash = last_tick_->state_sha256.empty() ? last_tick_->state_hash.get() : last_tick_->state_sha256;
    return core::Result<CompletedTick>::success({last_tick_->completed_tick, hash});
}

core::Result<Bytes> GalacticSessionAdapter::canonical_input() const {
    return core::Result<Bytes>::success(last_input_);
}

} // namespace eawr::net
