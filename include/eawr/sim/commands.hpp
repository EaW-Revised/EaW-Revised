#pragma once

#include "eawr/sim/math/geometry.hpp"

#include <compare>
#include <cstdint>
#include <variant>

namespace eawr::sim {

using EntityId = std::uint64_t;
using AssetId = std::uint64_t;

inline constexpr EntityId invalid_entity_id = 0;

struct EntityState {
    EntityId entity_id{};
    AssetId asset_id{};
    math::Vec3 position{};
    math::Vec3 velocity{};
    friend constexpr bool operator==(const EntityState&, const EntityState&) noexcept = default;
};

struct CommandKey {
    std::uint64_t tick{};
    std::uint32_t player_id{};
    std::uint64_t sequence{};
    friend constexpr auto operator<=>(const CommandKey&, const CommandKey&) noexcept = default;
};

struct CreateCommand {
    EntityState entity;
    friend constexpr bool operator==(const CreateCommand&, const CreateCommand&) noexcept = default;
};

struct DestroyCommand {
    EntityId entity_id{};
    friend constexpr bool operator==(const DestroyCommand&, const DestroyCommand&) noexcept = default;
};

struct SetPositionCommand {
    EntityId entity_id{};
    math::Vec3 position{};
    friend constexpr bool operator==(const SetPositionCommand&, const SetPositionCommand&) noexcept = default;
};

struct SetVelocityCommand {
    EntityId entity_id{};
    math::Vec3 velocity{};
    friend constexpr bool operator==(const SetVelocityCommand&, const SetVelocityCommand&) noexcept = default;
};

using CommandPayload = std::variant<
    CreateCommand,
    DestroyCommand,
    SetPositionCommand,
    SetVelocityCommand>;

struct Command {
    CommandKey key;
    CommandPayload payload;
    friend constexpr bool operator==(const Command&, const Command&) noexcept = default;
};

} // namespace eawr::sim
