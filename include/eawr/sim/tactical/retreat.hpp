#pragma once

#include "eawr/core/result.hpp"
#include "eawr/sim/tactical/types.hpp"
#include "eawr/sim/tactical/retreat_action.hpp"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace eawr::sim::tactical {

enum class RetreatPhase : std::uint8_t { idle, countdown, escaping, escaped };
enum class RetreatRefusal : std::uint8_t {
    none, invalid_player, wrong_mode, lockout, other_player, faction, tutorial,
    prevention, superweapon, forced, escape_started, battle_decided
};
struct RetreatPlayer {
    PlayerId player{};
    std::uint32_t countdown_frames{}; // truncated authored seconds * logical FPS (WR-44)
    bool playable{}, tutorial_allowed{true};
    friend bool operator==(const RetreatPlayer&, const RetreatPlayer&) = default;
};
struct RetreatRules {
    bool campaign{}, multiplayer_tactical{}, skirmish{};
    std::uint32_t lockout_frames{};
    std::vector<RetreatPlayer> players; // ascending participant IDs
    friend bool operator==(const RetreatRules&, const RetreatRules&) = default;
};
// Copied inputs from the owning world. The owner supplies the qualified locomotor predicate;
// omission never silently treats an unqualified actor as retreatable (WR-45 / U-8).
struct RetreatActor {
    EntityId id{};
    PlayerId owner{};
    bool live{}, locomotion{}, hyperspace{}, locomotor_allowed{};
    bool engines_present{}, engines_standing{true}, engines_disabled{}, permanent_disable{};
    bool team_member{}, carries_named_hero{}, persistent{};
    friend bool operator==(const RetreatActor&, const RetreatActor&) = default;
};
struct RetreatGate {
    bool prevention{}, superweapon{}, battle_decided{};
};
struct RetreatState {
    RetreatPhase phase{RetreatPhase::idle};
    PlayerId player{};
    std::uint64_t deadline{};
    bool forced{};
    bool station_wait{}, station_destroyed{};
    bool playback_reported{}, playback_playing{}; // reported means playback has ever existed this retreat
    std::uint32_t cinematic_frame{}, cinematic_duration{}; // active tactical frames, owned by the session
    std::vector<EntityId> actors; // source traversal order, not an ECS storage order
    friend bool operator==(const RetreatState&, const RetreatState&) = default;
};
struct RetreatView {
    RetreatState state;
    RetreatRefusal refusal{RetreatRefusal::none};
    PlayerId player{}; // perspective whose eligibility was queried
    bool auto_resolve_allowed{};
    std::uint32_t lockout_frames{}; // the rules' opening lockout, for the command-bar dial (HCB-15b)
};
struct RetreatPreventionSource {
    EntityId unit{};
    std::uint32_t ability_slot{}; // live active slot supplies WIN-03's source-keyed modifier
    std::vector<PlayerId> enemies;
};
[[nodiscard]] core::Result<void> validate_retreat_rules(const RetreatRules& rules);
[[nodiscard]] RetreatRefusal retreat_eligibility(const RetreatRules& rules,
    const RetreatState& state, PlayerId player, std::uint64_t frame, RetreatGate gate);
[[nodiscard]] RetreatRefusal request_retreat(const RetreatRules& rules, RetreatState& state,
    PlayerId player, RetreatAction action, std::uint64_t frame, RetreatGate gate,
    std::span<const RetreatActor> actors, bool forced = false);
// WR-45/46: countdown may cancel, then engine qualification is repeated at expiry.
// Escape completion belongs to the flight/station service and FC-R02's active-mode timer.
void service_retreat(const RetreatRules& rules, RetreatState& state, std::uint64_t frame,
    RetreatGate gate, std::span<const RetreatActor> actors);
[[nodiscard]] bool complete_retreat_escape(RetreatState& state);
struct RetreatFlightEffect { bool destroy_stations{}, completed{}; };
// WR-50..53 / FC-R02: frame is the owning simulation's prefix sample, never a command field.
[[nodiscard]] RetreatFlightEffect service_retreat_flight(RetreatState&, bool cinematics_enabled, std::uint32_t frame);
[[nodiscard]] bool midbattle_solve_allowed(const RetreatRules&, const RetreatState&, PlayerId, RetreatGate);

struct RetreatProtection {
    std::uint32_t category{};
    math::Fixed value{};
};
struct RetreatConfig {
    bool flight_qualified{true}; // an unavailable external locomotor owner may explicitly refuse
    RetreatRules rules;
    std::vector<RetreatActor> actors; // source actor order and identity/locomotor qualifications
    std::vector<RetreatPreventionSource> prevention;
    std::vector<EntityId> superweapons;
    math::Fixed attrition_factor{};
    std::vector<RetreatProtection> protection;
};
struct RetreatAttrition {
    std::vector<EntityId> destroyed; // reverse eligible actor traversal
    std::uint64_t draws{};
};
// WGT-36/43: category maxima sum; one synchronized draw per eligible persistent actor,
// even for a zero/negative threshold. draw15 must return a value in [0,32767].
[[nodiscard]] core::Result<RetreatAttrition> retreat_attrition(std::span<const RetreatActor> actors,
    PlayerId player, math::Fixed factor, std::span<const RetreatProtection> protection,
    const std::function<std::uint32_t()>& draw15);
[[nodiscard]] std::vector<std::uint8_t> encode_retreat_state(const RetreatState& state);
[[nodiscard]] core::Result<RetreatState> decode_retreat_state(std::span<const std::uint8_t> bytes);

} // namespace eawr::sim::tactical
