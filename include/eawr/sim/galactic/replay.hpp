#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/galactic/commands.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The galactic replay, EAWRGCR v1 (docs/galactic-replay-format.md): the tick-zero galaxy with its
// identities, seed, players, seats and options, every executed GameCommand in canonical
// (tick, seat, sequence) order, and the final tick. A recorded live run replays itself headless
// with identical per-tick hashes.
namespace eawr::sim::galactic {

inline constexpr std::uint16_t galactic_replay_format_version = 2;
inline constexpr std::uint32_t galactic_rules_version = 1;
inline constexpr std::size_t galactic_replay_max_bytes = 256U * 1024U * 1024U;
inline constexpr std::size_t galactic_replay_max_payload = 64U * 1024U;

// Ordered engine effects share the replay journal, while ordinary player
// command ordering stays (tick, seat, sequence). Barrier effects at frontier N
// precede world service N and follow completed world service N-1.
enum class NativeEffectPhase : std::uint8_t { barrier, world_service };
enum class NativeEffectKind : std::uint16_t {
    fleet = 1, launch = 2, land = 3, orbit_recheck = 4,
    production = 5, magic = 6, cash = 7, production_payment = 8, production_finish = 9,
    invasion = 10, actor_land = 11, technology = 12, technology_price = 13,
    ability_lifecycle = 14, tactical_built = 15, corruption_lifecycle = 16,
    planet_ruins = 17, // coordinator-reserved: shared live ruins adapter
    income_block = 18, // coordinator-reserved: WGEP-163 source-scoped income blockers
    production_lock = 19, // coordinator-reserved: story production lock (#683)
    story_reward = 20 // coordinator-reserved: story spawn and credits (#683); its replayer is registered by the campaign owner
};
struct NativeEffect {
    std::uint64_t frontier{}, sequence{};
    NativeEffectPhase phase{};
    NativeEffectKind kind{};
    std::vector<std::uint8_t> payload;
    bool operator==(const NativeEffect&) const = default;
};
struct GalacticReplay {
    GalacticSetup setup;
    std::uint64_t final_tick_count{};
    std::vector<GameCommand> commands;
    std::vector<NativeEffect> native_effects{};
    friend bool operator==(const GalacticReplay&, const GalacticReplay&) = default;
};

// The setup is valid, every command is a known payload for a listed seat in strictly increasing
// canonical order below the final tick, and the final tick is within the galactic tick limit.
[[nodiscard]] core::Result<void> validate_replay(const GalacticReplay& replay);
[[nodiscard]] core::Result<GalacticReplay> parse_replay(
    std::span<const std::uint8_t> bytes, std::string_view logical_path = {});
[[nodiscard]] core::Result<std::vector<std::uint8_t>> write_replay(const GalacticReplay& replay);

} // namespace eawr::sim::galactic
