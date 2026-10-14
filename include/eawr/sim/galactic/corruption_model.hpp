#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <array>
#include <optional>
#include <set>
#include <span>
#include <vector>

namespace eawr::sim::galactic::corruption_fixture {

using math::Fixed;
using Id = std::uint32_t;
using Frame = std::uint64_t;
inline constexpr auto one = Fixed::from_raw(Fixed::scale);

// Authored order, WGCH-04/50 and WGEP-13. None never indexes an authored slot.
enum class Type : std::uint8_t {
    none, intimidation, piracy, kidnapping, racketeering, militia, bribery,
    black_market, bonded_citizens
};
struct Transition {
    Type current{}, target{};
    bool increasing{}, decreasing{};
    Frame start{}, end{}, last_change{};
    [[nodiscard]] bool active() const { return increasing || decreasing; }
    friend bool operator==(const Transition&, const Transition&) = default;
};
struct Reward {
    Fixed credits;
    Id tactical_upgrade{}, build_unlock{};
    Fixed unlock_build_cost;
};
struct Player {
    Id id{}, faction{};
    Fixed credits;
    std::array<Id, 9> automatic_upgrades{};
    std::set<Id> upgrades, black_market_unlocks;
};
struct Planet {
    Transition transition;
    Id corrupting_player{}, target_allegiance{}, neutral_player{};
    std::array<Reward, 9> rewards{};
    std::set<Id> tactical_upgrades, build_unlocks;
    std::vector<Id> orbital_owners, landed_owners;
    unsigned ground_specials{}, space_specials{}, starbase_level{};
    std::vector<Fixed> political_modifiers;
    Fixed allegiance;
};
enum class Effect : std::uint8_t {
    refresh_visuals, story_increased, story_decreased, local_sound,
    add_automatic_upgrade, remove_automatic_upgrade, credit_reward,
    add_tactical_upgrade, remove_tactical_upgrade, add_build_unlock,
    remove_build_unlock, notify_abilities, clear_revolt_structures,
    surface_retreat, orbital_retreat, clear_political_modifiers, neutral_allegiance
};
struct Event {
    Effect effect{};
    Id player{}, value{};
    Type old_type{}, new_type{};
    friend bool operator==(const Event&, const Event&) = default;
};
// WGCH-11/51: false is rejected admission; invalid duration/deadline is diagnostic.
[[nodiscard]] core::Result<bool> begin(Transition&, Type target, Fixed seconds,
    std::uint32_t logical_fps, Frame now);
// WGCH-18/19: target allegiance gate, begin first, debit only on success.
[[nodiscard]] core::Result<bool> remove(Planet&, Player& actor, Fixed base_cost,
    Fixed cost_multiplier, Fixed seconds, std::uint32_t logical_fps, Frame now);
// WGCH-12..17: fixture mutations and ordered external interface requests.
struct SoundInput {
    Id owner{}, beneficiary{};
    bool owner_local{}, beneficiary_local{}, owner_event_present{}, beneficiary_event_present{};
};
// WGCH-75: local owner takes precedence; absent faction event starts nothing.
[[nodiscard]] std::optional<Id> completion_sound(const SoundInput&);
// local_sound is selected by completion_sound, independent of action success speech.
[[nodiscard]] core::Result<std::vector<Event>> complete(Planet&, std::vector<Player>&,
    Frame now, std::optional<Id> local_sound);

struct Siphon {
    bool active{};
    Id recipient{};
    Fixed fraction;
    Frame end{};
    friend bool operator==(const Siphon&, const Siphon&) = default;
};
struct IncomeInput {
    Type corruption{};
    Id owner{};
    Fixed pre_siphon_income, piracy_accrual;
    Siphon siphon;
    Frame now{};
    std::array<Fixed, 9> corruption_factors{};
};
struct IncomePlayer { Id id{}; bool benefits_from_corruption{}; bool eligible{true}; };
struct IncomeResult {
    std::vector<Fixed> payments;
    Fixed siphon_accrual, remaining_piracy;
    bool siphon_active{};
};
// WGEP-04/12/13: one planet, immutable evaluation, caller commits collection.
[[nodiscard]] core::Result<IncomeResult> income(const IncomeInput&,
    std::span<const IncomePlayer>, bool collect);
// WGCH-60: values are from flattened members' original types; no fleet-owner debit.
[[nodiscard]] core::Result<Fixed> accrue_piracy(Type, bool allied, bool stealth,
    Fixed accrued, std::span<const Fixed> original_type_values);
// WGEP-14: caller supplies synchronized unit draw; next-cycle factor only.
[[nodiscard]] core::Result<Fixed> next_income_factor(bool influenced, bool destroyed,
    Fixed current, Fixed minimum, Fixed maximum, Fixed unit_draw);
// WGEP-29/32: data factors, no literal stock economy values.
[[nodiscard]] Fixed production_factor(bool benefits, bool corrupted,
    Fixed ordinary, Fixed corruption);

struct MarketPlayer { Id id{}, faction{}; int displayed_tech{}; };
struct MarketItem {
    Id id{}, faction{};
    int tech{}, tutorial_tech{};
    Fixed base_cost, tutorial_base_cost, uncommon_multiplier{one};
    friend bool operator==(const MarketItem&, const MarketItem&) = default;
};
struct MarketContext {
    bool opposing_planet{}, playable_faction{}, black_market_corruption{};
    bool owner_present{}, tutorial{}, multiplayer{}, human_buyer{};
    Id owner_faction{}, buyer{};
    Fixed planet_modifier, price_modifier{one}, tech_purchase_multiplier{one};
};
struct Offer { Id item{}; Fixed price; bool owned{}; friend bool operator==(const Offer&, const Offer&) = default; };
// WGEP-58/WGCH-37: authored list order, faction filter, retain owned offers.
[[nodiscard]] core::Result<std::vector<Offer>> offers(const MarketContext&,
    std::span<const MarketItem>, std::span<const MarketPlayer>, const std::set<Id>& owned);
// WGEP-80: rounded UI affordability is separate from exact execution comparison.
[[nodiscard]] core::Result<bool> menu_enabled(const Offer&, Fixed credits);
// WGEP-55/80, WGCH-74: caller resolves current buyer owner into Player (prepared
// owner is not an input). Repeated unlocks still debit; success requests despawn.
[[nodiscard]] core::Result<bool> purchase(Player&, const Offer&, bool buyer_present,
    bool target_present);
// WGCH-38/71, WGEP-81: buyer displayed tech, not item tech. Invalid/empty -> zero.
[[nodiscard]] core::Result<Fixed> absence(int buyer_displayed_tech,
    std::span<const Fixed> minimum, std::span<const Fixed> maximum, Fixed unit_draw);

// Named seams: ownership interruption/restore and callback admission (WGCH U-01/U-04),
// actual retreat selection (WGM-61..63), stale menu delivery and zero-price AI
// selection (WGCH U-09). This fixture model does not schedule or invent those paths.
} // namespace eawr::sim::galactic::corruption_fixture
