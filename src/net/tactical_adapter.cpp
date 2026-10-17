#include "eawr/net/tactical_adapter.hpp"

#include <algorithm>
#include <utility>

namespace eawr::net {
namespace {
namespace tactical = sim::tactical;
using Bytes = std::vector<std::uint8_t>;
template<class T> core::Result<T> refused(std::string_view code, std::string message) {
    core::Diagnostic error;
    error.code = code;
    error.message = std::move(message);
    return core::Result<T>::failure(std::move(error));
}
tactical::TacticalSetup envelope_setup(const tactical::SeatBinding& binding) {
    tactical::TacticalSetup setup;
    setup.players.push_back({binding.player, 0, 0, tactical::player_flag_commandable});
    setup.queue_identities = true;
    return setup;
}
}

core::Result<Bytes> encode_tactical_input(sim::SeatCommandKey key, const tactical::SeatBinding& binding,
    const LocalIntent& intent) {
    if (key.tick >= tactical::max_ticks)
        return refused<Bytes>(session_errors::malformed, "tactical input tick exceeds session limit");
    auto admitted = tactical::admit_human_command(binding, {key, binding.player, intent.units, intent.payload});
    if (!admitted) return refused<Bytes>(session_errors::admission, admitted.error().message);
    auto encoded = tactical::write_replay({envelope_setup(binding), key.tick + 1, {admitted.value().command}});
    if (!encoded) return refused<Bytes>(session_errors::malformed, encoded.error().message);
    if (encoded.value().size() > max_batch_bytes - 41)
        return refused<Bytes>(session_errors::resource_limit, "tactical input exceeds frame byte budget");
    return encoded;
}

core::Result<tactical::SeatCommand> decode_tactical_input(const SeatInput& input, const tactical::SeatBinding& binding) {
    if (input.key.tick >= tactical::max_ticks || input.bytes.size() > max_batch_bytes)
        return refused<tactical::SeatCommand>(session_errors::malformed, "invalid tactical input size or tick");
    auto decoded = tactical::parse_replay(input.bytes);
    if (!decoded) return refused<tactical::SeatCommand>(session_errors::malformed, decoded.error().message);
    const auto& replay = decoded.value();
    if (replay.setup != envelope_setup(binding) || replay.final_tick_count != input.key.tick + 1 || replay.commands.size() != 1)
        return refused<tactical::SeatCommand>(session_errors::malformed, "input replay envelope is not canonical");
    const auto& command = replay.commands.front();
    if (command.key.tick != input.key.tick || command.key.sequence != input.key.sequence)
        return refused<tactical::SeatCommand>(session_errors::malformed, "opaque command stamp disagrees with seat envelope");
    tactical::SeatCommand intent{input.key, command.key.player_id, command.units, command.payload};
    auto admitted = tactical::admit_human_command(binding, intent);
    if (!admitted) return refused<tactical::SeatCommand>(session_errors::admission, admitted.error().message);
    const auto canonical = encode_tactical_input(input.key, binding, {command.units, command.payload});
    if (!canonical) return core::Result<tactical::SeatCommand>::failure(canonical.error());
    if (canonical.value() != input.bytes)
        return refused<tactical::SeatCommand>(session_errors::malformed, "noncanonical tactical command encoding");
    return core::Result<tactical::SeatCommand>::success(std::move(intent));
}

TacticalSessionAdapter::TacticalSessionAdapter(tactical::TacticalSession session, const sim::PartitionExecutor& executor,
    std::vector<tactical::SeatBinding> bindings)
    : session_(std::move(session)), executor_(&executor), bindings_(std::move(bindings)) {}
std::uint64_t TacticalSessionAdapter::completed_tick() const { return session_.completed_tick(); }
std::string TacticalSessionAdapter::state_hash() const { return session_.state_sha256(); }
Bytes TacticalSessionAdapter::canonical_state() const { return session_.canonical_state_bytes(); }
const tactical::SeatBinding* TacticalSessionAdapter::binding(sim::SeatId seat) const {
    const auto found = std::find_if(bindings_.begin(), bindings_.end(), [&](const auto& value) { return value.seat == seat; });
    return found == bindings_.end() ? nullptr : &*found;
}
core::Result<void> TacticalSessionAdapter::validate(const SeatInput& input) const {
    const auto* bound = binding(input.key.seat);
    if (!bound) return refused<void>(session_errors::admission, "tactical input has unbound seat");
    const auto command = decode_tactical_input(input, *bound);
    if (!command) return core::Result<void>::failure(command.error());
    return core::Result<void>::success();
}
core::Result<void> TacticalSessionAdapter::install(std::span<const SeatInput> canonical) {
    std::vector<tactical::SeatCommand> decoded;
    for (const auto& input : canonical) {
        const auto* bound = binding(input.key.seat);
        if (!bound) return refused<void>(session_errors::admission, "tactical input has unbound seat");
        auto command = decode_tactical_input(input, *bound);
        if (!command) return core::Result<void>::failure(command.error());
        decoded.push_back(std::move(command).value());
    }
    routed_.clear();
    for (const auto& command : decoded) {
        const auto* bound = binding(command.key.seat);
        if (std::holds_alternative<tactical::QuitPayload>(command.payload)) {
            // LAN-8B: leaving a shared owner removes this controller. The existing
            // faction departure policy applies only when its final controller leaves.
            const auto other = std::any_of(bindings_.begin(), bindings_.end(), [&](const auto& seat) {
                return seat.seat != bound->seat && seat.player == bound->player && seat.active && !seat.observer
                    && !departed_seats_.contains(seat.seat);
            });
            if (other) {
                departed_seats_.insert(bound->seat);
                continue;
            }
        }
        auto admitted = session_.submit_human(*bound, command, faction_sequences_[bound->player]++);
        if (!admitted) return refused<void>(session_errors::simulation, admitted.error().message);
        routed_.push_back(std::move(admitted).value().command);
        if (std::holds_alternative<tactical::QuitPayload>(command.payload))
            departed_seats_.insert(bound->seat);
    }
    std::sort(routed_.begin(), routed_.end(), [](const auto& left, const auto& right) { return left.key < right.key; });
    return core::Result<void>::success();
}
core::Result<Bytes> TacticalSessionAdapter::canonical_input() const {
    auto replay = session_.record();
    const auto tick = session_.completed_tick();
    std::erase_if(replay.commands, [tick](const auto& command) { return command.key.tick + 1 != tick; });
    std::erase_if(replay.object_turns, [tick](const auto& command) { return command.tick + 1 != tick; });
    if (replay.commands.empty() && replay.object_turns.empty()) return core::Result<Bytes>::success({});
    return tactical::write_replay(replay);
}
core::Result<CompletedTick> TacticalSessionAdapter::step() {
    const auto previous = session_.departure_controls().size();
    auto result = engine_step_ ? engine_step_(session_, *executor_) : session_.step(*executor_);
    if (!result) return refused<CompletedTick>(session_errors::simulation, result.error().message);
    last_tick_ = std::move(result).value();
    const auto controls = session_.departure_controls();
    for (std::size_t index = previous; index < controls.size(); ++index)
        if (controls[index].basic_ai && takeover_handler_) {
            auto attached = takeover_handler_(controls[index].player, controls[index].tick);
            if (!attached) return core::Result<CompletedTick>::failure(attached.error());
        }
    const auto hash = last_tick_->state_sha256.empty() ? last_tick_->state_hash.get() : last_tick_->state_sha256;
    return core::Result<CompletedTick>::success({last_tick_->completed_tick, hash});
}
std::optional<tactical::PlayerId> TacticalSessionAdapter::player_for_seat(sim::SeatId seat) const {
    const auto* found = binding(seat);
    return found ? std::optional<tactical::PlayerId>{found->player} : std::nullopt;
}
bool TacticalSessionAdapter::is_departure(const SeatInput& input) const {
    const auto* bound = binding(input.key.seat);
    if (!bound) return false;
    const auto command = decode_tactical_input(input, *bound);
    return command && std::holds_alternative<tactical::QuitPayload>(command.value().payload);
}

core::Result<TacticalLockstepSession> TacticalLockstepSession::create(const tactical::TacticalSetup& setup,
    const tactical::TacticalContent& content, MessageTransport& transport, const sim::PartitionExecutor& executor,
    std::vector<tactical::SeatBinding> bindings, SessionOptions options, SessionMonitor* monitor) {
    // MP-6 publication is owned by the generic session. This convenience owner
    // keeps a typed adapter and must adopt that publication seam before opting in.
    if (options.resync)
        return refused<TacticalLockstepSession>(resync_errors::options, "tactical convenience owner does not support resync publication");
    if (bindings.size() != transport.peer_count() || transport.self() >= bindings.size())
        return refused<TacticalLockstepSession>(session_errors::options, "tactical binding roster disagrees with transport");
    std::vector<PeerSeat> seats;
    for (const auto& bound : bindings) {
        const auto player = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const auto& value) { return value.player_id == bound.player; });
        if (player == setup.players.end() || (!bound.observer && (!bound.active || !player->commandable())))
            return refused<TacticalLockstepSession>(session_errors::options, "tactical seat is not bound to an active faction");
        seats.push_back({bound.seat, bound.active && !bound.observer});
    }
    const auto local = bindings[transport.self()];
    auto session = tactical::TacticalSession::create(setup, content);
    if (!session) return core::Result<TacticalLockstepSession>::failure(session.error());
    const auto roster = bindings;
    auto adapter = std::make_unique<TacticalSessionAdapter>(std::move(session).value(), executor, std::move(bindings));
    auto core = LockstepSession::create(*adapter, transport, std::move(seats), options, monitor);
    if (!core) return core::Result<TacticalLockstepSession>::failure(core.error());
    if (setup.departure_policy) core.value().enable_departures([roster](sim::SeatCommandKey key) -> core::Result<Bytes> {
        const auto found = std::find_if(roster.begin(), roster.end(), [&](const auto& bound) { return bound.seat == key.seat; });
        if (found == roster.end()) return refused<Bytes>(departure_errors::agreement, "unbound synthetic departure seat");
        return encode_tactical_input(key, *found, {{}, tactical::QuitPayload{}});
    });
    return core::Result<TacticalLockstepSession>::success(TacticalLockstepSession(std::move(adapter), std::move(core).value(), local));
}
core::Result<sim::SeatCommandKey> TacticalLockstepSession::submit(
    const LocalIntent& intent, const std::optional<std::uint64_t> input_epoch) {
    return core_.submit([&](sim::SeatCommandKey key) { return encode_tactical_input(key, local_, intent); }, input_epoch);
}
core::Result<TacticalPumpResult> TacticalLockstepSession::pump(std::uint64_t now_ms) {
    auto result = core_.pump(now_ms);
    if (!result) return core::Result<TacticalPumpResult>::failure(result.error());
    TacticalPumpResult tactical;
    if (result.value().completed) tactical.completed = adapter_->take_tick();
    for (const auto& event : result.value().departure_events) {
        const auto controls = adapter_->simulation().departure_controls();
        const auto found = std::find_if(controls.begin(), controls.end(), [&](const auto& value) { return value.tick == event.key.tick && value.player == adapter_->player_for_seat(event.key.seat); });
        // The seat binding is recovered from the replayed control record below; a
        // policy-free adapter still exposes its legacy quit list for existing callers.
        if (found == controls.end()) continue;
        const bool human = std::any_of(adapter_->bindings().begin(), adapter_->bindings().end(), [&](const auto& bound) {
            return bound.active && !bound.observer && bound.player == found->controller;
        });
        DepartureOutcome outcome{event, found->player, found->controller,
            found->basic_ai || !human ? DepartureState::continues_with_ai : DepartureState::continues};
        if (event.key.seat == local_.seat) outcome.state = DepartureState::match_ended;
        else if (const auto& battle = adapter_->simulation().outcome();
            battle && battle->condition == tactical::VictoryCondition::intentional_quit) {
            const auto players = adapter_->simulation().players();
            const auto local = std::find_if(players.begin(), players.end(), [&](const auto& value) { return value.player_id == local_.player; });
            outcome.state = local != players.end() && local->team_id == battle->winner_team
                ? DepartureState::won_by_departure : DepartureState::match_ended;
        }
        tactical.departure_outcomes.push_back(outcome);
        outcomes_.push_back(outcome);
    }
    tactical.quits = std::move(result).value().departures;
    return core::Result<TacticalPumpResult>::success(std::move(tactical));
}

} // namespace eawr::net
