#pragma once

#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace eawr::presentation::godot_backend {

namespace live_session {

struct BattleEnd final {
    ui::BattleResult result{ui::BattleResult::victory};
    std::uint64_t decided_tick{};
    std::uint64_t end_tick{};
    std::uint64_t shown_frame{};
    std::optional<std::uint64_t> ended_frame;
};

struct DrawDigestRow final {
    std::uint64_t frame{};
    std::uint64_t tick{};
    std::int64_t presented_milli{};
    std::size_t visible{};
    std::size_t drawn{};
    std::uint64_t visible_digest{};
    std::uint64_t drawn_digest{};
};

struct IonShotRow final {
    std::uint64_t switched_on{};
    std::uint64_t first_on{};
    std::uint64_t last_on{};
    bool on{};
};

struct IonStunRow final {
    std::uint64_t first{};
    std::uint32_t max_frames{};
};

struct ArrivalRow final {
    sim::tactical::PlayerId owner{};
    sim::tactical::TypeId type{};
    std::uint64_t first_tick{};
    std::uint64_t visible_tick{};
    std::uint64_t landed_tick{};
    std::uint32_t last_frame{};
    double facing_yaw{};
};

struct SpinRow final {
    std::uint64_t started{};
    std::optional<std::uint64_t> ended;
};

} // namespace live_session
} // namespace eawr::presentation::godot_backend
