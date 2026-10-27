#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/free_space.hpp"
#include "eawr/sim/tactical/pads.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/type_flags.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <map>
#include <span>
#include <string>
#include <vector>

// Skirmish purchasing (#530, docs/behaviour/space-purchasing.md): credits and income (PU-01 to
// PU-07), the station build queues (PU-10 to PU-22) and the hyperspace arrival of bought units
// (PU-30 to PU-39). Everything here is a pure function of Q24 values and integers: no clock,
// thread or host input.
namespace eawr::sim::tactical {

enum class BuildQueue : std::uint8_t {
    units = 0,    // Tactical_Units
    upgrades = 1, // Tactical_Upgrades
};
inline constexpr std::uint32_t build_queue_count = 2;

// What a completed build becomes (PU-19). A unit joins its owner's reinforcement pool; an upgrade
// (a station upgrade or level-up, #540) or a structure (a build-pad object, #541) is recorded for
// its station until the rules that apply it exist.
enum class BuildKind : std::uint8_t {
    unit = 0,
    upgrade = 1,
    structure = 2,
};

// WPR-33: absent limits allow a build; zero explicitly forbids it.
struct BuildRequirements {
    std::optional<std::uint32_t> lifetime_player;
    std::optional<std::uint32_t> current_player;
    std::optional<std::uint32_t> lifetime_allies;
    std::optional<std::uint32_t> current_allies;
    std::vector<TypeId> prerequisites;
    friend bool operator==(const BuildRequirements&, const BuildRequirements&) = default;
};

// WPR-51: percentages, in health, damage, energy, shield, defense, speed order, then the hardpoint
// fire range (WFR-01: the debug build's fire-range modifier list, uncapped).
using CombatBonuses = std::array<math::Fixed, 7>;
struct UpgradeBonus {
    std::uint32_t stacking_category{};
    std::vector<TypeId> applicable;
    CombatBonuses percentages{};
    friend bool operator==(const UpgradeBonus&, const UpgradeBonus&) = default;
};
// WHE-13..19/53: immutable, prequalified type lists; the source owns its recipient ledger.
struct CommandBonusProfile {
    TypeId type{};
    std::uint32_t slot{};
    UpgradeBonus bonus;
    FactionId specific_faction{};
    bool battlefield{}, all_allies{}; // WGT-50: player ledger, not a recipient-type filter
    math::Fixed reveal_delta{};
    std::vector<PlayerId> enabled_players; // WGT-49: explicit locks override authored default
    bool apply_to_self{}; // WHE-53: the debug build's creation flag defaults off
    friend bool operator==(const CommandBonusProfile&, const CommandBonusProfile&) = default;
};
struct IncomeModifier {
    TypeId target_source{};
    std::uint32_t stacking_category{};
    math::Fixed percentage{};
    math::Fixed additive{};
    math::Fixed interval_percentage{};
    bool all_allies{};
    bool reverse{};
    friend bool operator==(const IncomeModifier&, const IncomeModifier&) = default;
};

// WST-03: one cloak-duration contribution, (Stealth_Duration_Multiplier - 1), to its holder's owner
// (and, with all_allies, the owner's allies). Category maximum, categories summed.
struct StealthDurationModifier {
    std::uint32_t stacking_category{};
    math::Fixed percentage{};
    bool all_allies{};
    friend bool operator==(const StealthDurationModifier&, const StealthDurationModifier&) = default;
};

// WRB-05: Enable_Ability on an upgrade object: the nested ability (by its authored name, lower case)
// it switches on for the listed types, for its holder and, when `all_allies`, the holder's allies.
struct AbilityEnableRule {
    std::string ability_name;
    std::vector<TypeId> applicable; // sorted, unique
    bool all_allies{};
    friend bool operator==(const AbilityEnableRule&, const AbilityEnableRule&) = default;
};

struct UpgradeProfile {
    struct Bounty {
        math::Fixed percentage{};
        std::int32_t category{};
        std::vector<FactionId> factions{};
        bool all_allies{};
        friend bool operator==(const Bounty&, const Bounty&) = default;
    };
    TypeId type{};
    bool level_up{};
    bool increments_tech{};
    TypeId removes_previous{};
    std::vector<UpgradeBonus> bonuses;
    std::vector<IncomeModifier> income_modifiers{};
    std::vector<StealthDurationModifier> stealth_duration{};
    std::vector<AbilityEnableRule> ability_enables{}; // WRB-05
    struct SpecialUnlock {
        bool all_allies{}, reverse{};
        friend bool operator==(const SpecialUnlock&, const SpecialUnlock&) = default;
    };
    std::vector<SpecialUnlock> special_unlocks{};
    bool consume_after_special_fire{};
    std::vector<Bounty> bounties{};
    friend bool operator==(const UpgradeProfile&, const UpgradeProfile&) = default;
};

// One entry of a station's build list (PU-10 to PU-13, PU-21).
struct BuildOption {
    TypeId type{};
    BuildKind kind{BuildKind::unit};
    BuildQueue queue{BuildQueue::units};
    math::Fixed price{};             // PU-12, whole credits
    std::uint32_t build_frames{};    // PU-13, a human player's
    std::uint32_t ai_build_frames{}; // PU-13 with the AI difficulty's multiplier
    std::uint32_t population{};      // PU-21 Population_Value
    bool available{};                // PU-20: false for an entry M2 shows but never builds
    BuildRequirements requirements{};
    std::string disabled_reason{};    // RG-04: content metadata, outside ledger bytes
    std::vector<TypeId> higher_upgrades{}; // WPR-63: presentation-only successor closure
    friend bool operator==(const BuildOption&, const BuildOption&) = default;
};

// A station type's build list for one faction (PU-10), in list order. A station that levels up
// changes type, and so its menu (#540); nothing here is per faction or per level.
struct StationMenu {
    TypeId station{};
    FactionId faction{};
    std::vector<BuildOption> options;
    TypeId next_level{}; // WPR-52: Next_Level_Base
    bool station_producer{true}; // PU-11: pad menus cannot use the station queue route
    [[nodiscard]] const BuildOption* find(TypeId type) const noexcept;
    friend bool operator==(const StationMenu&, const StationMenu&) = default;
};

inline constexpr std::uint32_t always_on = 0xffffffffU;

// A bonus to an income stream (PU-04): what each recipient gains per frame on top, while the
// hardpoint with index `hardpoint` of the source's HardPoints stands (always_on: always).
struct IncomeBonus {
    math::Fixed per_frame{};
    std::uint32_t hardpoint{always_on};
    friend constexpr bool operator==(const IncomeBonus&, const IncomeBonus&) noexcept = default;
};

// A type whose live units pay an income stream (PU-02 to PU-05): the station's, and later a
// mining facility's (#541). Each recipient gains the base plus the standing bonuses per frame;
// a player's income is the sum over every live source it receives from.
struct IncomeProfile {
    TypeId source{};
    math::Fixed per_frame{};
    std::vector<IncomeBonus> bonuses;
    bool split_with_allies{true};
    bool full_amount_to_everyone{true};
    math::Fixed base_value{};       // WBP-44: authored value before modifiers
    math::Fixed interval_seconds{};
    friend bool operator==(const IncomeProfile&, const IncomeProfile&) = default;
};

// WBP-44: absence is distinct from zero, so a category containing only negative
// contributions retains its greatest signed value. Neutral contributions do not register.
struct IncomeCategory {
    std::uint32_t category{};
    std::array<std::optional<math::Fixed>, 3> winners{};
};

void reduce_income_modifier(const IncomeModifier& modifier, std::span<IncomeCategory> categories);
[[nodiscard]] core::Result<math::Fixed> modified_income_per_frame(math::Fixed base_value,
    math::Fixed base_interval, std::span<const IncomeCategory> categories);

// WBP-30/31: only types with tactical-sale behavior enter this table.
struct PadSaleProfile {
    TypeId type{};
    math::Fixed percentage{math::Fixed::from_raw(math::Fixed::scale / 2)};
    friend bool operator==(const PadSaleProfile&, const PadSaleProfile&) = default;
};

// WTA-45: an AI player's production time under its difficulty's Space_Build_Time_Multiplier. A queue entry's
// frames round to nearest (PU-13) and stay at least 1; a pad's whole seconds truncate (WBP-15). A multiplier of
// 1 returns the authored value unchanged.
[[nodiscard]] std::uint32_t scaled_ai_frames(std::uint32_t frames, math::Fixed multiplier) noexcept;
[[nodiscard]] std::uint32_t scaled_ai_seconds(std::uint32_t seconds, math::Fixed multiplier) noexcept;

// A lobby player's economy (PU-01, PU-14, PU-18, PU-21, PU-34).
struct EconomyPlayer {
    PlayerId player{};
    math::Fixed credits{};           // starting credits
    std::uint32_t population_cap{};  // Space_Tactical_Unit_Cap of its faction
    bool ai{};                       // no queue limit; refunded when an entry fails (PU-18)
    math::Fixed reinforcement_yaw{}; // degrees: the facing its reinforcements arrive with (PU-34)
    std::uint32_t start_tech{};       // WPR-02
    std::uint32_t max_tech{};
    math::Fixed credit_multiplier{math::Fixed::from_raw(math::Fixed::scale)}; // WPR-12: positive AI changes only
    // WTA-45: the AI difficulty's Space_Build_Time_Multiplier on this AI player's production time; 1 is Normal.
    math::Fixed ai_build_time_multiplier{math::Fixed::from_raw(math::Fixed::scale)};
    std::optional<math::Fixed> credit_cap{}; // WGT-27: absent in skirmish
    std::optional<math::Fixed> strategic_credits{}; // WGA-53/54: retained AI balance
    friend constexpr bool operator==(const EconomyPlayer&, const EconomyPlayer&) noexcept = default;
};

// A type whose non-allied units keep reinforcements out of a planar circle (PU-31).
struct PreventionProfile {
    TypeId type{};
    math::Fixed radius{};
    friend constexpr bool operator==(const PreventionProfile&, const PreventionProfile&) noexcept = default;
};

// WR-22/23: shared by authoritative placement and its type-less deployment overlay.
[[nodiscard]] bool reinforcement_prevention_blocks(const math::Vec3& centre,
    math::Fixed radius, const math::Vec3& point) noexcept;
[[nodiscard]] bool reinforcement_inside_bounds(const std::optional<std::array<math::Fixed, 4>>& bounds,
    const math::Vec3& point) noexcept;

// A type's footprint for a bought unit's arrival: its placement box (space-movement PL-02) and its
// height (LZ-01). The arrival's free-space search blocks on every live unit that has a box (PL-08).
struct FootprintProfile {
    TypeId type{};
    std::optional<PlacementBox> box; // none: created on the point and never blocking (PL-02)
    math::Fixed layer_z{};           // Layer_Z_Adjust: a created unit is raised by it (LZ-01), 0 when none
    friend constexpr bool operator==(const FootprintProfile&, const FootprintProfile&) noexcept = default;
};

// The economy content of a skirmish. Like the other content tables it is neither replay data nor
// state; a session without it (empty()) has no economy and hashes exactly as before #530.
struct HeroDeployment {
    TypeId purchase{}, deployed{};
    struct Rider {
        TypeId type{};
        bool named{}, generic{};
        friend bool operator==(const Rider&, const Rider&) = default;
    };
    std::vector<Rider> riders; // WHE-49: authored order, hero members only
    friend bool operator==(const HeroDeployment&, const HeroDeployment&) = default;
};

struct EconomyRules {
    bool campaign{}; // WGT-26/28: identity reserves, campaign costs and restrictions
    SkirmishMatchPolicy match_policy{};        // replay binding, not a ledger field
    PadRules pads;                            // WBP-01..19: object services, never producer queues
    std::vector<EconomyPlayer> players;        // strictly increasing player ID
    std::vector<StationMenu> menus;            // strictly increasing (station, faction)
    std::vector<IncomeProfile> income;         // strictly increasing source type
    std::vector<PreventionProfile> prevention; // strictly increasing type
    std::vector<FootprintProfile> footprints;  // strictly increasing type: every unit and map object type
    std::vector<HeroDeployment> heroes; // WHE-49: strictly increasing purchase type
    std::vector<std::pair<TypeId, std::uint32_t>> campaign_population; // WGT-19/28; no ship menu needed
    std::vector<FactionId> campaign_playable_factions; // WGT-116: includes background allies without a command seat
    TypeFlags disabled_types;                 // RG-04: indexed flags, sorted IDs for validation
    std::uint32_t max_queue{5};                // PU-14: a human player's queue length
    // PU-31: the playable bounds in the plane, when the map declares them.
    std::optional<std::array<math::Fixed, 4>> bounds; // min x, min y, max x, max y
    math::Fixed vulnerability{};               // PU-38: the arrival's defense modifier (FoC -3)
    std::uint32_t vulnerability_frames{};      // PU-38: its duration in frames (FoC 150)
    math::Fixed collision_distance{};          // WR-25: Space_Reinforcement_Collision_Check_Distance
    std::vector<UpgradeProfile> upgrades; // WPR-22, WPR-51, WPR-52, increasing type
    std::vector<CommandBonusProfile> command_bonuses; // WHE-53: sorted (type, declared slot)
    std::vector<PadSaleProfile> pad_sales; // increasing completed type
    std::vector<std::pair<TypeId, math::Fixed>> bounty_values{}; // WGCH-65: current type, selected mode
    bool bounty_collection_enabled{true};
    // Optional fixture/controller boundary, in completed ticks. Rules stay immutable;
    // economic commits use the effective human/AI policy for their current tick.
    std::map<PlayerId, std::uint64_t> player_ai_start_ticks{};
    [[nodiscard]] bool empty() const noexcept { return players.empty(); }
    [[nodiscard]] const EconomyPlayer* player(PlayerId id) const noexcept;
    [[nodiscard]] std::optional<EconomyPlayer> player_at(PlayerId id, std::uint64_t tick) const noexcept;
    [[nodiscard]] const StationMenu* menu(TypeId station, FactionId faction) const noexcept;
    // WPR-22/33: the menu a producer offers a buyer. An ordinary producer lists its owner's faction group
    // (the purchase gate's rule, also for an allied buyer); a construction pad lists the builder's.
    [[nodiscard]] const StationMenu* producer_menu(TypeId producer, FactionId owner_faction, FactionId buyer_faction) const noexcept;
    [[nodiscard]] const IncomeProfile* stream(TypeId source) const noexcept;
    [[nodiscard]] const PadSaleProfile* pad_sale(TypeId type) const noexcept;
    [[nodiscard]] const PreventionProfile* prevention_of(TypeId type) const noexcept;
    [[nodiscard]] const FootprintProfile* footprint_of(TypeId type) const noexcept;
    [[nodiscard]] const HeroDeployment* hero(TypeId purchase) const noexcept {
        const auto found = std::lower_bound(heroes.begin(), heroes.end(), purchase,
            [](const HeroDeployment& entry, const TypeId type) { return entry.purchase < type; });
        return found != heroes.end() && found->purchase == purchase ? &*found : nullptr;
    }
    [[nodiscard]] const UpgradeProfile* upgrade(TypeId type) const noexcept;
    friend bool operator==(const EconomyRules&, const EconomyRules&) = default;
};

// Fails with EAWR-SIM-0305 unless every economy player is a declared player, the lists are
// strictly increasing, credits, amounts and radii are nonnegative and bounded, every available
// option has a positive price and build time, max_queue is positive and the bounds are ordered.
[[nodiscard]] std::vector<IncomeCategory> initial_income_categories(const EconomyRules& rules, std::size_t partitions);

[[nodiscard]] core::Result<void> validate_economy(const EconomyRules& rules, std::span<const Player> players);

// --- State ---------------------------------------------------------------------------------------

// A queued build (PU-14 to PU-18): the price paid and the frames it takes; the front entry also
// holds the frame it completes in, every other entry zero.
struct QueueEntry {
    TypeId type{};
    EntityId station{};
    math::Fixed paid{};
    std::uint32_t frames{};
    std::uint64_t complete_frame{};
    std::uint64_t entry_id{}; // PU-17: stable across queue shifts, never reused within a player's session.
    friend constexpr bool operator==(const QueueEntry&, const QueueEntry&) noexcept = default;
};

// A completed upgrade or structure and the station that built it (PU-19; #540, #541).
struct IncomeModifierState {
    bool initialized{};
    std::uint64_t next_scan_frame{};
    std::vector<EntityId> attached;
    friend bool operator==(const IncomeModifierState&, const IncomeModifierState&) = default;
};

struct CompletedBuild {
    TypeId type{};
    EntityId station{};
    EntityId object{}; // WPR-22: hidden upgrade object held by its station, stable ID
    std::vector<IncomeModifierState> income_modifiers{};
    std::optional<std::uint64_t> level_up_service_frame{}; // WSL-31: initialized at held-object creation
    friend constexpr bool operator==(const CompletedBuild&, const CompletedBuild&) noexcept = default;
};

// A player's economy state: credits, the two queues (BuildQueue order), the reinforcement pool
// of completed units in completion order (PU-30) and the completed upgrades and structures.
struct PlayerEconomy {
    PlayerId player{};
    math::Fixed credits{};
    std::array<std::vector<QueueEntry>, build_queue_count> queues;
    std::vector<TypeId> pool;
    std::vector<CompletedBuild> completed;
    // WBP-45/46: reverse termination applies its effects, without periodic service.
    // Retired sources do not count as owned objects or satisfy production prerequisites.
    std::vector<CompletedBuild> income_residuals{};
    // PU-71: presentation metadata, deliberately excluded from canonical ledger bytes.
    std::uint64_t pool_version{};
    std::uint64_t pool_additions{};
    std::uint64_t pool_addition_frame{};
    std::map<TypeId, std::uint64_t> lifetime; // WPR-22 step 1
    std::uint32_t tech_level{};
    // SAE-11: completion-order identities, independent of the logical purchase type.
    std::vector<std::uint64_t> pool_tokens{};
    std::uint64_t next_pool_token{1};
    std::uint64_t next_queue_entry_id{1};
    math::Fixed campaign_extra_releases{}; // WGA-54: authoritative credit bridge
    math::Fixed collected_bounty{}; // WGCH-65: rounded payments, independent of credit cap
    friend bool operator==(const PlayerEconomy&, const PlayerEconomy&) = default;
};

struct ProductionCounts {
    std::uint64_t owned_player{};
    std::uint64_t current_player{};
    std::uint64_t lifetime_player{};
    std::uint64_t current_allies{};
    std::uint64_t lifetime_allies{};
    std::uint64_t queued_player{}; // WPR-61: query metadata, outside state/hash
    std::uint64_t queued_allies{};
    std::uint64_t owned_allies{}; // SAE-12: live/held allied ownership, excluding queues and pools; query metadata.
};
// WPR-33: counts include queued entries; buying counts one more, a validity sweep does not.
template <typename Counts>
[[nodiscard]] bool production_allowed(const BuildOption& option, const bool buying, Counts&& counts) {
    const auto count = counts(option.type);
    const auto within = [buying](const std::optional<std::uint32_t>& limit, const std::uint64_t value) {
        return !limit || (*limit != 0 && (buying ? value < *limit : value <= *limit));
    };
    const auto& r = option.requirements;
    const auto lifetime_allows = [](const std::optional<std::uint32_t>& limit, const std::uint64_t built) {
        return !limit || built < *limit;
    };
    if (!option.available || !lifetime_allows(r.lifetime_player, count.lifetime_player)
        || !within(r.current_player, count.current_player) || !lifetime_allows(r.lifetime_allies, count.lifetime_allies)
        || !within(r.current_allies, count.current_allies)) return false;
    // WPR-33: allied live/held and pooled objects satisfy prerequisites; queued ones do not.
    for (const auto type : r.prerequisites) {
        const auto prerequisite = counts(type);
        if (prerequisite.current_allies <= prerequisite.queued_allies) return false;
    }
    return true;
}

// PU-15: queues `option` at `station` for `player` at `frame` and pays its price. Returns the
// reason it was refused (queue full for a human, too few credits), or none.
[[nodiscard]] RejectReason queue_build(PlayerEconomy& state, const EconomyPlayer& player, const EconomyRules& rules,
    const BuildOption& option, EntityId station, std::uint64_t frame, bool prepaid = false);

// WPR-12: positive AI changes scale by difficulty; debits floor at zero. Skirmish is uncapped.
[[nodiscard]] core::Result<void> change_credits(PlayerEconomy& state, const EconomyPlayer& player, math::Fixed amount);
// PU-17: removes entry `index` of `queue` at `frame` and refunds its price; the next entry
// becomes the front when the front was removed. False when there is no such entry.
[[nodiscard]] core::Result<bool> cancel_build(PlayerEconomy& state, const EconomyPlayer& player, BuildQueue queue, std::uint32_t index, std::uint64_t frame);
// PU-17, PU-63, WPR-31: a stale identity is refused, without falling back to its former slot.
[[nodiscard]] core::Result<bool> cancel_build_entry(PlayerEconomy& state, const EconomyPlayer& player, BuildQueue queue, std::uint64_t entry_id, std::uint64_t frame);

// PU-18, PU-16: one frame's service of both queues. `producible` says whether an entry can still
// be produced; one that cannot is removed (refunded for an AI player). `kind` says what an
// entry's type is: a completed unit joins the pool, anything else the completed list, in queue
// order.
template <typename Producible, typename Kind, typename Complete>
core::Result<void> service_production(PlayerEconomy& state, const EconomyPlayer& player, const std::uint64_t frame,
    Producible&& producible, Kind&& kind, Complete&& complete) {
    for (auto& queue : state.queues) {
        for (std::size_t index = queue.size(); index-- > 0;) {
            if (producible(queue[index])) continue;
            if (player.ai) {
                const auto refunded = change_credits(state, player, queue[index].paid);
                if (!refunded) return refunded;
            }
            const bool front = index == 0;
            queue.erase(queue.begin() + static_cast<std::ptrdiff_t>(index));
            if (front && !queue.empty()) queue.front().complete_frame = frame + queue.front().frames;
        }
        if (queue.empty() || frame < queue.front().complete_frame) continue;
        ++state.lifetime[queue.front().type]; // WPR-22, before creating the completed object
        if (kind(queue.front()) == BuildKind::unit) {
            state.pool.push_back(queue.front().type);
            state.pool_tokens.push_back(state.next_pool_token++);
            ++state.pool_version;
            ++state.pool_additions;
            state.pool_addition_frame = frame;
        } else {
            state.completed.push_back(CompletedBuild{queue.front().type, queue.front().station});
        }
        complete(queue.front());
        queue.erase(queue.begin());
        if (!queue.empty()) queue.front().complete_frame = frame + queue.front().frames;
    }
    return core::Result<void>::success();
}

template <typename Producible, typename Kind>
core::Result<void> service_production(PlayerEconomy& state, const EconomyPlayer& player, const std::uint64_t frame,
    Producible&& producible, Kind&& kind) {
    return service_production(state, player, frame, producible, kind, [](const QueueEntry&) {});
}

// PU-21: population shares are counted in units of 1/population_share_scale, so a squadron's
// craft of up to 16 members share their squadron's value exactly.
inline constexpr std::int64_t population_share_scale = 720720;
// The count of a sum of shares, any fraction rounded up.
[[nodiscard]] std::uint32_t population_count(std::int64_t shares) noexcept;
// One craft's share of a squadron of `members` craft whose type counts `population`.
[[nodiscard]] std::int64_t population_share(std::uint32_t population, std::uint32_t members) noexcept;

// --- Arrival (PU-34 to PU-39) --------------------------------------------------------------------

inline constexpr std::uint32_t arrival_frames = 150;        // PU-35: frame 150 ends it
inline constexpr std::uint32_t arrival_visible_frame = 35;  // PU-36, PU-37
inline constexpr std::uint32_t arrival_reveal_frame = 120;  // WR-39, debug build
inline constexpr std::uint32_t arrival_sweep_frames = 115;  // WR-25, debug build

// An arriving unit: the arrival frames it has flown (0 in the frame it was created), the point it
// arrives at and its unit facing in the plane.
struct ArrivalState {
    std::uint32_t frame{};
    math::Vec3 exit{};
    math::Vec3 direction{};
    friend constexpr bool operator==(const ArrivalState&, const ArrivalState&) noexcept = default;
};

// PU-35: the distance an arriving unit still flies after arrival frame `frame` (0 to 149); the
// whole lane D at frame 0 and 0 from frame 149. Each d(k) is rounded to Q24, and the tail is the
// exact sum of the rounded steps.
[[nodiscard]] math::Fixed arrival_tail(std::uint32_t frame) noexcept;
// Where an arriving unit stands after `state.frame` frames: exit Ã¢Ë†â€™ direction Ãƒâ€” tail.
[[nodiscard]] core::Result<math::Vec3> arrival_position(const ArrivalState& state);
// PU-37, PU-38: how a projectile hit is taken by a unit whose arrival is `arrival` (null: not
// arriving). Nothing, the hit dropped, while it is hidden (arrival frames 0 to 34); else the
// defense modifier to add, the rules' vulnerability while the arrival frame is below
// vulnerability_frames and 0 after. This movement-record helper describes the arrival phase;
// the session's independent WR-41 timer preserves longer authored durations after landing.
[[nodiscard]] std::optional<math::Fixed> arrival_hit_defense(const ArrivalState* arrival, const EconomyRules& rules) noexcept;
// The unit vector in the plane of a yaw in degrees.
[[nodiscard]] core::Result<math::Vec3> planar_direction(math::Fixed yaw_degrees);

// --- Presentation --------------------------------------------------------------------------------

// A player's economy in a snapshot (#530): credits, population (PU-21), both queues with each
// entry's frames and, for the front, its completion frame, and the pool.
struct ReserveView {
    std::uint64_t token{};
    TypeId type{};
    std::uint32_t population{}; // WGT-19/28: original purchase type, including retained variants
    bool restricted{}; // WR-09: qualified campaign location restriction
    friend bool operator==(const ReserveView&, const ReserveView&) = default;
};
struct EconomyView {
    PlayerId player{};
    math::Fixed credits{};
    std::uint32_t population{};
    std::uint32_t population_cap{};
    std::array<std::vector<QueueEntry>, build_queue_count> queues;
    std::vector<TypeId> pool;
    std::uint64_t pool_version{};
    std::uint64_t pool_additions{};
    std::uint64_t pool_addition_frame{};
    std::vector<CompletedBuild> completed{};
    std::map<TypeId, std::uint64_t> lifetime{};
    std::uint32_t tech_level{};
    std::vector<std::uint64_t> pool_tokens{};
    friend bool operator==(const EconomyView&, const EconomyView&) = default;
    bool special_weapons_enabled{}; // WSW-12/46: per player; manual hardpoint availability remains separate.
    math::Fixed campaign_extra_releases{};
    std::vector<ReserveView> reserves{}; // local-owner rows, in current pool order
};

} // namespace eawr::sim::tactical
