#pragma once

#include "eawr/sim/math/fixed.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Typed fixture contracts and live victory-monitor adapter. Rules are scoped to
// docs/behaviour/walks/gc-story.md and WGS-170..179 in gc-session-save.md.
// The plot engine owns event counters, graph eligibility and WST-U01 table order.
namespace eawr::sim::galactic::story {
using Player = std::uint32_t;
using Object = std::uint64_t;
enum class Travel { enter, move_hero, land, deploy_hero, fleet_bounced, invasion_bounced, planet_destroyed };
enum class Economy { construct, construct_level, credits, tech_level, era };
enum class Loss { destroy, destroy_base };
enum class Battle { won, lost, retreat, victory, mission_lost };
enum class Corruption { changed, complete, failed };
enum class Text { ai_notification, speech_done, generic, click_gui, open_corruption, black_market, sabotage };
enum class View { select_planet, zoom_in, zoom_out };
// WST-35/38/39 declarations only: M4 emitters/evaluators are not implemented here.
enum class Tactical { destroy, space_started, land_started, reinforce, capture_hero, defeat_hero,
    garrison, attack_hardpoint, select_unit, arrived, full_stop, command, guard, proximity,
    fog_object, fog_position, capture_structure, load_map };
enum class BaseKind { ground, space, either };
enum class OwnerFilter { any, friendly, enemy };
struct Fleet {
    Player owner{};
    std::vector<std::string> types;
    std::vector<std::string> heroes; // Includes heroes inside transports (WST-31).
    std::vector<std::string> orbit_types;
    bool stealth{}, raid_capable{};
};
struct TravelNotice { Travel kind{}; std::string planet; Fleet fleet; std::optional<std::string> hero; };
struct EconomyNotice { Economy kind{}; std::int64_t value{}; std::string type, planet; BaseKind base{BaseKind::either}; Player object_owner{}; };
struct LossNotice { Loss kind{}; std::string type, planet; BaseKind base{BaseKind::either}; Player object_owner{}; };
struct BattleNotice { Battle kind{}; std::string planet, faction, plot; BaseKind base{BaseKind::either}; Player actor{}; std::vector<std::string> ship_classes; Object object{}; };
struct CorruptionNotice { Corruption kind{}; std::string planet, corruption; };
struct ConquestNotice { std::string planet; Player new_owner{}, previous_owner{}; Object object{}; }; // WST-32.
struct ConquestCountNotice { std::string planet; Object object{}; }; // WGM-192: distinct second producer
struct TextNotice { Text kind{}; std::string text; std::optional<std::string> planet; };
struct ViewNotice { View kind{}; std::string planet; };
struct ElapsedNotice { math::Fixed time_seconds; bool paused{}; }; // WST-11/12 mode time, including fractions.
struct MovieDoneNotice {};
struct TacticalNotice { Tactical kind{}; std::string type, planet, child_story; Player actor{}; };
using Payload = std::variant<TravelNotice, EconomyNotice, LossNotice, BattleNotice, CorruptionNotice,
    TextNotice, ViewNotice, ElapsedNotice, MovieDoneNotice, ConquestNotice, TacticalNotice, ConquestCountNotice>;
struct Notification {
    std::uint64_t sequence{}; // Feed-local strictly increasing identity; copied with fixture state.
    std::optional<Player> player; // WST-14: absence broadcasts, never player zero as a sentinel.
    Payload payload;
};
[[nodiscard]] std::string_view event_name(const Payload& payload);
// WST-30: trigger/flag polling belongs to the engine, not an external notifier.
// WST-32/33: one ownership/construction notice may evaluate multiple engine event families.
[[nodiscard]] std::string_view notification_seam(const Payload& payload);
// WST-34: report both final and original types when different, preserving notification order.
[[nodiscard]] std::vector<LossNotice> destruction(std::string type, std::string original_type,
    std::string planet, Player owner);
struct DispatchGate {
    bool foreground_state{true}, attract_demo{}, active_mode{true};
    bool foreground{true}, multiplayer{}, replay{}, single_step{};
};
struct Recipient { std::string plot; Player owner{}; bool active{true}; };
struct Delivery { std::string plot; Notification notification; };
struct FeedState { std::uint64_t last_sequence{}; std::vector<Notification> pending; };
class FakeFeed {
public:
    explicit FakeFeed(FeedState state = {});
    // WST-13/14. Returned callbacks run synchronously before the next submit.
    [[nodiscard]] std::vector<Delivery> submit(Notification notification, const DispatchGate& gate,
        std::span<const Recipient> recipients);
    [[nodiscard]] std::vector<Delivery> service(const DispatchGate& gate, std::span<const Recipient> recipients);
    [[nodiscard]] const FeedState& state() const noexcept { return state_; }
private:
    FeedState state_;
};

// WST-37: AI notifications with and without a planet are distinct predicates.
[[nodiscard]] bool matches_ai(const TextNotice& notice, std::span<const std::string> planets,
    std::span<const std::string> notifications);
struct AdmissionEvent {
    bool subplot_active{true}, active{true}, triggered{}, disabled{};
    Player owner{};
    Travel kind{Travel::enter};
    bool load_tactical_map{}; // WST-35/52 distinct from travel events.
    bool link_tactical{};
    std::vector<std::string> planets, required_types, orbit_types, heroes;
    OwnerFilter filter{OwnerFilter::any};
    bool allow_stealth{}, exclusive{}, raid{};
    BaseKind map_kind{BaseKind::either};
};
struct Admission { bool restricted_entry{}, special_space_map{}, special_land_map{}; };
struct OwnerRelation { Player event_owner{}; bool friendly{}; };
// Caller resolves friendly/enemy relations into this immutable query input.
[[nodiscard]] Admission admission(std::span<const AdmissionEvent> events, const TravelNotice& arrival,
    std::span<const OwnerRelation> relations, BaseKind kind);

struct Planet {
    std::string name;
    Player owner{};
    std::int64_t x{}, y{}; // Fixture metric only; live nearest-planet resolver is a named seam.
    std::vector<Player> orbit_owners;
    bool revealed{}, restricted{}, restrict_autoresolve{}, restrict_corruption{}, restrict_sabotage{}, restrict_black_market{}, restrict_abilities{};
};
struct UnitType { std::string name, faction; std::int64_t tech{}; bool fights_in_space{}; };
struct ProductionType { std::string type; bool locked{}, disabled{}; };
struct PlayerState { Player id{}; std::string faction; bool ai{}, local_human{}; std::int64_t tech{}, max_tech{}, credits{}; std::vector<ProductionType> production; };
struct Unit { Object id{}; std::string type, planet; Player owner{}; bool live{true}, free_store{}; };
struct VictoryState { std::uint64_t waits{}; std::optional<Player> winner; std::int64_t delay_seconds{}; bool suppress_text{}, test_requested{}; };
struct FixtureState {
    std::vector<Planet> planets;
    std::vector<PlayerState> players;
    std::vector<UnitType> types;
    std::vector<Unit> units;
    Object next_object{1};
    VictoryState victory;
};
enum class ProductionSwitch { buildable, lock, disable, enable };
enum class Restriction { entry, autoresolve, corruption, sabotage, black_market, abilities, reveal };
struct Spawn { std::string type, planet; std::int64_t count{1}; };
struct ProductionReward { ProductionSwitch kind{}; std::vector<std::string> types; bool lock{}; };
struct CreditsReward { std::int64_t amount{}; };
struct TechReward { std::string faction; std::int64_t level{}; };
struct RestrictionReward { Restriction kind{}; std::string planet; bool enabled{}; };
// SET_PLANET_RESTRICTED options 3/4 and other non-stock layouts require WST-U09 evidence.
// This fixture Boolean is the common on/off effect, not a parser for those layouts.
struct VictoryReward { std::string faction; bool suppress_text{}; };
struct VictoryWaitReward { bool enable{}; bool supplied{true}; }; // Absent/empty parameter is inert, WGS-174.
struct DeferredReward { std::string name; std::vector<std::string> authored_parameters; };
using Reward = std::variant<Spawn, ProductionReward, CreditsReward, TechReward, RestrictionReward,
    VictoryReward, VictoryWaitReward, DeferredReward>;
enum class EffectKind { spawned, free_store, human_flash, guarding_fleets, orbit_relink,
    credits_changed, tech_changed, victory_test, production_changed, restriction_changed };
struct Effect { EffectKind kind{}; Object object{}; std::string planet; std::int64_t duration_milliseconds{}; std::optional<Payload> feedback; };
// Fleet/flash effects are ordered service requests. Live fleet unification, first-fleet relink,
// conflict admission and presentation callbacks remain at their owning-service seams.
enum class RewardStatus { applied, inert, unverified, invalid };
struct RewardResult { RewardStatus status{RewardStatus::applied}; std::string diagnostic; std::vector<Effect> effects; };
// WST-20: invoke only after the engine's Lua trigger callback. Transactional on invalid input.
[[nodiscard]] RewardResult apply(FixtureState& state, Player owner, const Reward& reward);
} // namespace eawr::sim::galactic::story
