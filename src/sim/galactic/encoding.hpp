#pragma once

#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/galactic/events.hpp"
#include "eawr/sim/galactic/history.hpp"
#include "eawr/sim/galactic/types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Canonical little-endian encodings of the galaxy's state sections and snapshot parts. The state
// hash, the snapshot hash and (later) the checkpoint's galaxy sections share them.
namespace eawr::sim::galactic::detail {

inline constexpr std::array<std::uint8_t, 8> state_magic{'E', 'A', 'W', 'R', 'G', 'S', 'T', 0};
inline constexpr std::array<std::uint8_t, 8> snapshot_magic{'E', 'A', 'W', 'R', 'G', 'S', 'N', 0};
inline constexpr std::uint32_t state_encoding_version = 10;
inline constexpr std::uint16_t section_version = 1;

void put_u8(std::vector<std::uint8_t>& bytes, std::uint8_t value);
void put_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value);
void put_u32(std::vector<std::uint8_t>& bytes, std::uint32_t value);
void put_u64(std::vector<std::uint8_t>& bytes, std::uint64_t value);
void put_i64(std::vector<std::uint8_t>& bytes, std::int64_t value);
void put_bytes(std::vector<std::uint8_t>& bytes, std::span<const std::uint8_t> value);
void encode_history(std::vector<std::uint8_t>&, const HistoryArchive&);
void encode_history_snapshot(std::vector<std::uint8_t>&, const HistorySnapshot&);

void encode_players(std::vector<std::uint8_t>& bytes, std::span<const GalacticPlayer> players);
void encode_player_economy(std::vector<std::uint8_t>& bytes, const PlayerEconomySetup& economy);
void encode_fiscal(std::vector<std::uint8_t>& bytes, const FiscalSetup& fiscal);
void encode_income(std::vector<std::uint8_t>&, const IncomeSetup&);
void encode_income_state(std::vector<std::uint8_t>&, const IncomeState&);
void encode_economy_corruption(std::vector<std::uint8_t>&, const EconomyCorruptionState&);
void encode_ability_setup(std::vector<std::uint8_t>&, const GalacticAbilitySetup&);
[[nodiscard]] std::optional<GalacticAbilitySetup> decode_ability_setup(std::span<const std::uint8_t>);
void encode_ability_state(std::vector<std::uint8_t>&, const GalacticAbilityState&);
void encode_prepared_ability(std::vector<std::uint8_t>&, const PreparedAbility&);
[[nodiscard]] std::optional<PreparedAbility> decode_prepared_ability(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<EconomyCorruptionState> decode_economy_corruption(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<IncomeSetup> decode_income(std::span<const std::uint8_t>);
void encode_census(std::vector<std::uint8_t>&, const PopulationCensus&);
void encode_population(std::vector<std::uint8_t>&, std::span<const PopulationTotal>);
[[nodiscard]] std::optional<PopulationCensus> decode_census(std::span<const std::uint8_t>);
void encode_production(std::vector<std::uint8_t>& bytes, const ProductionSetup& production);
// Hash domain 8 replaces each immutable offer table with its SHA-256 digest.
// Replay/checkpoint/canonical-state encodings continue to carry the complete tables.
using ProductionOfferDigests = std::vector<std::array<std::uint8_t, 32>>;
[[nodiscard]] ProductionOfferDigests production_offer_digests(const ProductionSetup&);
void encode_production_hash(std::vector<std::uint8_t>&, const ProductionSetup&, const ProductionOfferDigests&);
// Mutable admission inputs, excluding offer content and queue payment/countdown fields.
void encode_production_menu_inputs(std::vector<std::uint8_t>&, const ProductionSetup&, PlayerId,
    std::span<const PlanetId> visible);
[[nodiscard]] std::optional<ProductionSetup> decode_production(std::span<const std::uint8_t> bytes);
void encode_observer(std::vector<std::uint8_t>&, const ObserverSetup&);
void encode_planet_access(std::vector<std::uint8_t>&, const PlanetAccessState&);
[[nodiscard]] std::optional<PlanetAccessState> decode_planet_access(std::span<const std::uint8_t>);
[[nodiscard]] std::optional<ObserverSetup> decode_observer(std::span<const std::uint8_t>);
void encode_observer_command(std::vector<std::uint8_t>&, const ObserverCommand&);
[[nodiscard]] std::optional<ObserverCommand> decode_observer_command(std::span<const std::uint8_t>);
void encode_planet(std::vector<std::uint8_t>& bytes, const Planet& planet);
void encode_route(std::vector<std::uint8_t>& bytes, const Route& route);
void encode_fleet(std::vector<std::uint8_t>& bytes, const Fleet& fleet);
// Extension/state payload; legacy fleet row encoding is unchanged.
void encode_fleet_control(std::vector<std::uint8_t>& bytes, const FleetControlSetup& control,
    std::span<const Fleet> fleets);
void encode_movement(std::vector<std::uint8_t>& bytes, const MovementSetup& inputs, std::span<const Fleet> fleets);
struct DecodedMovement {
    MovementSetup inputs;
    std::vector<std::pair<FleetId, FleetMotion>> fleets;
};
[[nodiscard]] core::Result<DecodedMovement> decode_movement(std::span<const std::uint8_t> bytes);
void encode_control(std::vector<std::uint8_t>& bytes, const PoliticalControlSetup& inputs);
[[nodiscard]] core::Result<PoliticalControlSetup> decode_control(std::span<const std::uint8_t> bytes);
void encode_event(std::vector<std::uint8_t>& bytes, const GalacticEvent& event);
void encode_seat_control(std::vector<std::uint8_t>& bytes, const SeatControlSetup& control);
void encode_seat_control_state(std::vector<std::uint8_t>& bytes, const SeatControlState& state);

// Raw digest bytes of a lowercase hex SHA-256.
[[nodiscard]] std::array<std::uint8_t, 32> digest_bytes(std::string_view hex) noexcept;

[[nodiscard]] core::Diagnostic diagnostic(std::string_view code, std::string message);

} // namespace eawr::sim::galactic::detail
