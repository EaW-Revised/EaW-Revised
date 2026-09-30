#include "eawr/sim/world.hpp"

#include "replay_internal.hpp"

#include "../../third_party/entt/single_include/entt/entt.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace eawr::sim {
namespace {

struct StableId {
    EntityId value{};
};

struct AssetBinding {
    AssetId value{};
};

struct Position {
    math::Vec3 value{};
};

struct Velocity {
    math::Vec3 value{};
};

[[nodiscard]] core::Diagnostic diagnostic(const std::string_view code, std::string message) {
    return core::Diagnostic{
        .code = std::string(code),
        .severity = core::Severity::error,
        .message = std::move(message),
        .logical_path = std::nullopt,
        .line = std::nullopt,
        .column = std::nullopt,
        .source_id = std::string("simulation-v1"),
    };
}

[[nodiscard]] std::string command_context(
    const std::size_t index,
    const Command& command,
    const std::string_view message) {
    return "command " + std::to_string(index) + " at tick "
        + std::to_string(command.key.tick) + ": " + std::string(message);
}

[[nodiscard]] core::Result<math::Fixed> scale_raw_rational(
    const math::Fixed value,
    const std::uint32_t numerator,
    const std::uint32_t denominator) {
    const auto raw = value.raw();
    const bool negative = raw < 0;
    const std::uint64_t magnitude = negative
        ? (std::uint64_t{0} - static_cast<std::uint64_t>(raw))
        : static_cast<std::uint64_t>(raw);

    const std::uint64_t low_product = (magnitude & 0xffffffffU) * numerator;
    const std::uint64_t high_product = (magnitude >> 32U) * numerator;
    std::array<std::uint32_t, 3> product{};
    product[0] = static_cast<std::uint32_t>(low_product);
    const std::uint64_t middle = (high_product & 0xffffffffU) + (low_product >> 32U);
    product[1] = static_cast<std::uint32_t>(middle);
    product[2] = static_cast<std::uint32_t>((high_product >> 32U) + (middle >> 32U));

    std::array<std::uint32_t, 3> quotient{};
    std::uint64_t remainder{};
    for (std::size_t offset = product.size(); offset != 0; --offset) {
        const auto index = offset - 1;
        const auto current = (remainder << 32U) | product[index];
        quotient[index] = static_cast<std::uint32_t>(current / denominator);
        remainder = current % denominator;
    }
    const bool round_up = remainder * 2U > denominator
        || (remainder * 2U == denominator && (quotient[0] & 1U) != 0U);
    if (round_up) {
        std::uint64_t carry = 1;
        for (auto& limb : quotient) {
            const auto sum = static_cast<std::uint64_t>(limb) + carry;
            limb = static_cast<std::uint32_t>(sum);
            carry = sum >> 32U;
        }
        if (carry != 0) {
            return core::Result<math::Fixed>::failure(diagnostic(
                diagnostic_codes::movement_overflow,
                "rational displacement overflows the Q24 raw domain"));
        }
    }
    if (quotient[2] != 0) {
        return core::Result<math::Fixed>::failure(diagnostic(
            diagnostic_codes::movement_overflow,
            "rational displacement overflows the Q24 raw domain"));
    }
    const auto result_magnitude = (static_cast<std::uint64_t>(quotient[1]) << 32U)
        | quotient[0];
    const auto positive_limit = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto negative_limit = std::uint64_t{1} << 63U;
    if ((!negative && result_magnitude > positive_limit)
        || (negative && result_magnitude > negative_limit)) {
        return core::Result<math::Fixed>::failure(diagnostic(
            diagnostic_codes::movement_overflow,
            "rational displacement overflows the Q24 raw domain"));
    }
    std::int64_t result{};
    if (!negative) {
        result = static_cast<std::int64_t>(result_magnitude);
    } else if (result_magnitude == negative_limit) {
        result = std::numeric_limits<std::int64_t>::min();
    } else {
        result = -static_cast<std::int64_t>(result_magnitude);
    }
    return core::Result<math::Fixed>::success(math::Fixed::from_raw(result));
}

[[nodiscard]] std::uint64_t splitmix64(std::uint64_t& state) noexcept {
    state += 0x9e3779b97f4a7c15ULL;
    auto value = state;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

[[nodiscard]] math::Mat3x4 translation_matrix(const math::Vec3& position) noexcept {
    auto matrix = math::identity_matrix();
    matrix.rows[0][3] = position.x;
    matrix.rows[1][3] = position.y;
    matrix.rows[2][3] = position.z;
    return matrix;
}

} // namespace

class World::Impl final {
public:
    explicit Impl(const Replay& source)
        : replay(source), rng_state(source.seed) {
        if (source.initial_entities.empty()) {
            next_id = 1;
        } else if (source.initial_entities.back().entity_id == std::numeric_limits<EntityId>::max()) {
            next_id = invalid_entity_id;
        } else {
            next_id = source.initial_entities.back().entity_id + 1;
        }
        rebuild(source.initial_entities, false);
        publish_snapshot();
    }

    [[nodiscard]] std::vector<EntityState> sorted_entities() const {
        std::vector<EntityState> result;
        result.reserve(handles.size());
        for (const auto& [id, handle] : handles) {
            result.push_back(EntityState{
                id,
                registry.get<AssetBinding>(handle).value,
                registry.get<Position>(handle).value,
                registry.get<Velocity>(handle).value,
            });
        }
        return result;
    }

    void rebuild(const std::vector<EntityState>& states, const bool reverse) {
        registry.clear();
        handles.clear();
        const auto insert = [this](const EntityState& state) {
            const auto handle = registry.create();
            registry.emplace<StableId>(handle, state.entity_id);
            registry.emplace<AssetBinding>(handle, state.asset_id);
            registry.emplace<Position>(handle, state.position);
            registry.emplace<Velocity>(handle, state.velocity);
            handles.emplace(state.entity_id, handle);
        };
        if (reverse) {
            for (auto iterator = states.rbegin(); iterator != states.rend(); ++iterator) {
                insert(*iterator);
            }
        } else {
            for (const auto& state : states) {
                insert(state);
            }
        }
    }

    void publish_snapshot() {
        std::vector<RenderInstance> instances;
        instances.reserve(handles.size());
        for (const auto& entity : sorted_entities()) {
            instances.push_back(RenderInstance{
                entity.entity_id,
                entity.asset_id,
                translation_matrix(entity.position),
            });
        }
        current_snapshot = std::make_shared<const RenderSnapshot>(completed_tick, std::move(instances));
    }

    [[nodiscard]] std::vector<std::uint8_t> canonical_bytes() const {
        static constexpr std::array<std::uint8_t, 8> magic{'E', 'A', 'W', 'R', 'S', 'T', 'A', 0};
        const auto states = sorted_entities();
        std::vector<std::uint8_t> bytes;
        bytes.reserve(120 + states.size() * 64);
        bytes.insert(bytes.end(), magic.begin(), magic.end());
        detail::append_u32(bytes, state_encoding_version);
        detail::append_u32(bytes, simulation_rules_version);
        detail::append_u32(bytes, 1);
        detail::append_u32(bytes, math::Fixed::fractional_bits);
        detail::append_u32(bytes, replay.tick_numerator);
        detail::append_u32(bytes, replay.tick_denominator);
        detail::append_u64(bytes, completed_tick);
        detail::append_u64(bytes, replay.final_tick_count);
        detail::append_u64(bytes, next_id);
        detail::append_u64(bytes, rng_state);
        detail::append_u64(bytes, tick_nonce);
        bytes.insert(bytes.end(), replay.content_identity.begin(), replay.content_identity.end());
        detail::append_u64(bytes, states.size());
        for (const auto& state : states) {
            detail::append_entity(bytes, state);
        }
        const auto pending_count = replay.commands.size() - next_command_index;
        detail::append_u64(bytes, pending_count);
        for (auto index = next_command_index; index < replay.commands.size(); ++index) {
            detail::append_command(bytes, replay.commands[index]);
        }
        return bytes;
    }

    Replay replay;
    entt::registry registry;
    std::map<EntityId, entt::entity> handles;
    std::uint64_t completed_tick{};
    EntityId next_id{};
    std::uint64_t rng_state{};
    std::uint64_t tick_nonce{};
    std::size_t next_command_index{};
    std::shared_ptr<const RenderSnapshot> current_snapshot;
};

std::size_t InlineExecutor::worker_count() const noexcept {
    return 1;
}

core::Result<void> InlineExecutor::execute(
    const std::size_t partition_count,
    const std::function<void(std::size_t)>& partition) const {
    for (std::size_t index = 0; index < partition_count; ++index) {
        partition(index);
    }
    return core::Result<void>::success();
}

World::World(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
World::World(World&&) noexcept = default;
World& World::operator=(World&&) noexcept = default;
World::~World() = default;

core::Result<World> World::create(const Replay& replay) {
    const auto valid = detail::validate_replay_contract(replay, {});
    if (!valid) {
        return core::Result<World>::failure(valid.error());
    }
    return core::Result<World>::success(World(std::make_unique<Impl>(replay)));
}

core::Result<TickResult> World::step(const PartitionExecutor& executor) {
    if (impl_->completed_tick >= impl_->replay.final_tick_count) {
        return core::Result<TickResult>::failure(diagnostic(
            diagnostic_codes::simulation_complete,
            "cannot step after final tick " + std::to_string(impl_->replay.final_tick_count)));
    }
    if (executor.worker_count() == 0) {
        return core::Result<TickResult>::failure(diagnostic(
            diagnostic_codes::worker_failure,
            "executor reports no workers"));
    }

    std::map<EntityId, EntityState> staged;
    for (const auto& entity : impl_->sorted_entities()) {
        staged.emplace(entity.entity_id, entity);
    }
    auto staged_next_id = impl_->next_id;
    auto staged_command_index = impl_->next_command_index;
    while (staged_command_index < impl_->replay.commands.size()
        && impl_->replay.commands[staged_command_index].key.tick == impl_->completed_tick) {
        const auto& command = impl_->replay.commands[staged_command_index];
        std::optional<core::Diagnostic> command_error;
        std::visit([&](const auto& payload) {
            using Payload = std::decay_t<decltype(payload)>;
            if constexpr (std::is_same_v<Payload, CreateCommand>) {
                if (staged_next_id == invalid_entity_id) {
                    command_error = diagnostic(
                        diagnostic_codes::id_exhausted,
                        command_context(staged_command_index, command, "stable entity ID space is exhausted"));
                } else if (payload.entity.entity_id != staged_next_id) {
                    command_error = diagnostic(
                        diagnostic_codes::invalid_command,
                        command_context(staged_command_index, command,
                            "create ID " + std::to_string(payload.entity.entity_id)
                                + " does not equal next ID " + std::to_string(staged_next_id)));
                } else if (staged.contains(payload.entity.entity_id)) {
                    command_error = diagnostic(
                        diagnostic_codes::invalid_command,
                        command_context(staged_command_index, command, "create ID is already live"));
                } else {
                    staged.emplace(payload.entity.entity_id, payload.entity);
                    staged_next_id = staged_next_id == std::numeric_limits<EntityId>::max()
                        ? invalid_entity_id
                        : staged_next_id + 1;
                }
            } else if constexpr (std::is_same_v<Payload, DestroyCommand>) {
                if (staged.erase(payload.entity_id) != 1) {
                    command_error = diagnostic(
                        diagnostic_codes::invalid_command,
                        command_context(staged_command_index, command,
                            "destroy references unknown entity " + std::to_string(payload.entity_id)));
                }
            } else if constexpr (std::is_same_v<Payload, SetPositionCommand>) {
                const auto found = staged.find(payload.entity_id);
                if (found == staged.end()) {
                    command_error = diagnostic(
                        diagnostic_codes::invalid_command,
                        command_context(staged_command_index, command,
                            "set-position references unknown entity " + std::to_string(payload.entity_id)));
                } else {
                    found->second.position = payload.position;
                }
            } else {
                const auto found = staged.find(payload.entity_id);
                if (found == staged.end()) {
                    command_error = diagnostic(
                        diagnostic_codes::invalid_command,
                        command_context(staged_command_index, command,
                            "set-velocity references unknown entity " + std::to_string(payload.entity_id)));
                } else {
                    found->second.velocity = payload.velocity;
                }
            }
        }, command.payload);
        if (command_error) {
            return core::Result<TickResult>::failure(std::move(*command_error));
        }
        ++staged_command_index;
    }

    auto staged_rng_state = impl_->rng_state;
    const auto staged_tick_nonce = splitmix64(staged_rng_state);
    std::vector<EntityState> inputs;
    inputs.reserve(staged.size());
    for (const auto& [id, entity] : staged) {
        static_cast<void>(id);
        inputs.push_back(entity);
    }
    auto moved = inputs;
    std::vector<std::optional<core::Diagnostic>> movement_errors(inputs.size());
    const auto execute_result = executor.execute_phase("movement", tick_partition_count, [&](const std::size_t partition) {
        const auto range = partition_range(partition, inputs.size());
        for (auto index = range.begin; index < range.end; ++index) {
            const std::array<math::Fixed, 3> positions{
                inputs[index].position.x, inputs[index].position.y, inputs[index].position.z};
            const std::array<math::Fixed, 3> velocities{
                inputs[index].velocity.x, inputs[index].velocity.y, inputs[index].velocity.z};
            std::array<math::Fixed, 3> results{};
            for (std::size_t component = 0; component < 3; ++component) {
                const auto displacement = scale_raw_rational(
                    velocities[component], impl_->replay.tick_numerator, impl_->replay.tick_denominator);
                if (!displacement) {
                    movement_errors[index] = diagnostic(
                        diagnostic_codes::movement_overflow,
                        "tick " + std::to_string(impl_->completed_tick) + " entity "
                            + std::to_string(inputs[index].entity_id) + " component "
                            + std::to_string(component) + ": displacement overflow");
                    break;
                }
                const auto position = math::add(positions[component], displacement.value());
                if (!position) {
                    movement_errors[index] = diagnostic(
                        diagnostic_codes::movement_overflow,
                        "tick " + std::to_string(impl_->completed_tick) + " entity "
                            + std::to_string(inputs[index].entity_id) + " component "
                            + std::to_string(component) + ": position overflow");
                    break;
                }
                results[component] = position.value();
            }
            if (!movement_errors[index]) {
                moved[index].position = {results[0], results[1], results[2]};
            }
        }
    });
    if (!execute_result) {
        return core::Result<TickResult>::failure(execute_result.error());
    }
    for (auto& error : movement_errors) {
        if (error) {
            return core::Result<TickResult>::failure(std::move(*error));
        }
    }

    impl_->rebuild(moved, false);
    impl_->next_id = staged_next_id;
    impl_->rng_state = staged_rng_state;
    impl_->tick_nonce = staged_tick_nonce;
    impl_->next_command_index = staged_command_index;
    ++impl_->completed_tick;
    const auto committed_hash = state_sha256();
    impl_->publish_snapshot();
    return core::Result<TickResult>::success(TickResult{
        impl_->completed_tick,
        committed_hash,
        impl_->current_snapshot,
    });
}

std::uint64_t World::completed_tick() const noexcept { return impl_->completed_tick; }
std::uint64_t World::final_tick_count() const noexcept { return impl_->replay.final_tick_count; }
EntityId World::next_entity_id() const noexcept { return impl_->next_id; }
std::uint64_t World::rng_state() const noexcept { return impl_->rng_state; }
std::uint64_t World::tick_nonce() const noexcept { return impl_->tick_nonce; }
std::vector<EntityState> World::entities() const { return impl_->sorted_entities(); }

std::string World::state_sha256() const {
    const auto bytes = impl_->canonical_bytes();
    return sha256_hex(bytes);
}

std::shared_ptr<const RenderSnapshot> World::snapshot() const noexcept {
    return impl_->current_snapshot;
}

void World::scramble_storage_for_testing() {
    const auto states = impl_->sorted_entities();
    impl_->rebuild(states, true);
}

} // namespace eawr::sim
