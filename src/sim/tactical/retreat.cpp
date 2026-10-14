#include "eawr/sim/tactical/retreat.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace eawr::sim::tactical {
namespace {
template<typename T> core::Result<T> fail(std::string message) {
    core::Diagnostic diagnostic; diagnostic.code = std::string(diagnostic_codes::invalid_setup);
    diagnostic.message = std::move(message); return core::Result<T>::failure(std::move(diagnostic));
}
bool eligible(const RetreatActor& actor, const PlayerId player) {
    return actor.id && actor.owner == player && actor.live && actor.locomotion
        && actor.hyperspace && actor.locomotor_allowed && !actor.permanent_disable
        && (!actor.engines_present || actor.engines_standing);
}
void put(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned size) {
    for (unsigned i = 0; i < size; ++i) { bytes.push_back(static_cast<std::uint8_t>(value)); value >>= 8U; }
}
}
core::Result<void> validate_retreat_rules(const RetreatRules& rules) {
    PlayerId previous{};
    if (rules.players.size() > 64) return fail<void>("retreat participant limit");
    for (const auto& player : rules.players) {
        if (!player.player || player.player <= previous || !player.countdown_frames)
            return fail<void>("retreat participants need unique ordered identities and a countdown");
        previous = player.player;
    }
    return core::Result<void>::success();
}
RetreatRefusal retreat_eligibility(const RetreatRules& rules, const RetreatState& state,
    const PlayerId player, const std::uint64_t frame, const RetreatGate gate) {
    const auto found = std::find_if(rules.players.begin(), rules.players.end(),
        [&](const auto& row) { return row.player == player; });
    if (!player || found == rules.players.end()) return RetreatRefusal::invalid_player;
    if (!rules.campaign || rules.multiplayer_tactical || rules.skirmish) return RetreatRefusal::wrong_mode;
    if (gate.battle_decided) return RetreatRefusal::battle_decided;
    if (frame < rules.lockout_frames) return RetreatRefusal::lockout;
    if (state.phase != RetreatPhase::idle && state.player != player) return RetreatRefusal::other_player;
    if (!found->playable) return RetreatRefusal::faction;
    if (!found->tutorial_allowed) return RetreatRefusal::tutorial;
    if (gate.prevention) return RetreatRefusal::prevention;
    if (gate.superweapon) return RetreatRefusal::superweapon;
    return RetreatRefusal::none;
}
RetreatRefusal request_retreat(const RetreatRules& rules, RetreatState& state,
    const PlayerId player, const RetreatAction action, const std::uint64_t frame,
    const RetreatGate gate, const std::span<const RetreatActor> actors, const bool forced) {
    if (action < RetreatAction::start || action > RetreatAction::auto_resolve) return RetreatRefusal::wrong_mode;
    if (action == RetreatAction::auto_resolve)
        return midbattle_solve_allowed(rules, state, player, gate) ? RetreatRefusal::none : RetreatRefusal::wrong_mode;
    if (action == RetreatAction::cancel) {
        if (state.phase == RetreatPhase::idle || state.player != player) return RetreatRefusal::other_player;
        if (state.forced) return RetreatRefusal::forced;
        if (state.phase != RetreatPhase::countdown) return RetreatRefusal::escape_started;
        state = {}; return RetreatRefusal::none;
    }
    const auto refusal = retreat_eligibility(rules, state, player, frame, gate);
    if (refusal != RetreatRefusal::none) return refusal;
    if (state.phase != RetreatPhase::idle) return RetreatRefusal::escape_started;
    if (action == RetreatAction::auto_resolve) return RetreatRefusal::none; // solver owner, WAR-65
    const auto found = std::find_if(rules.players.begin(), rules.players.end(),
        [&](const auto& row) { return row.player == player; });
    if (frame > std::numeric_limits<std::uint64_t>::max() - found->countdown_frames)
        return RetreatRefusal::lockout;
    state.phase = RetreatPhase::countdown; state.player = player;
    state.deadline = frame + found->countdown_frames; state.forced = forced;
    for (const auto& actor : actors) if (eligible(actor, player)) state.actors.push_back(actor.id);
    return RetreatRefusal::none;
}
bool midbattle_solve_allowed(const RetreatRules& rules, const RetreatState& state,
    const PlayerId player, const RetreatGate gate) {
    return rules.campaign && !rules.multiplayer_tactical && !rules.skirmish && !gate.battle_decided
        && state.phase == RetreatPhase::idle && std::any_of(rules.players.begin(), rules.players.end(),
            [&](const auto& row) { return row.player == player && row.playable && row.tutorial_allowed; });
}
RetreatFlightEffect service_retreat_flight(RetreatState& state, const bool cinematics_enabled, const std::uint32_t frame) {
    RetreatFlightEffect effect;
    if (state.phase != RetreatPhase::escaping || frame < state.cinematic_frame) return effect;
    state.cinematic_frame = frame;
    // WR-50 absence comes only from the immutable seat setting. Reports are presentation state.
    const bool absent_at_initialization = frame == 0 && !cinematics_enabled;
    if (frame < 90 && !absent_at_initialization) return effect;
    // FC-R02: enabled seats finish at start + duration, even with absent/stopped/stuck reports.
    if (absent_at_initialization || frame >= state.cinematic_duration) {
        effect.completed = complete_retreat_escape(state); return effect;
    }
    state.station_wait = true;
    if (frame >= 181 && !state.station_destroyed) {
        state.station_destroyed = true; effect.destroy_stations = true;
    }
    return effect;
}
void service_retreat(const RetreatRules& rules, RetreatState& state, const std::uint64_t frame,
    const RetreatGate gate, const std::span<const RetreatActor> actors) {
    if (state.phase != RetreatPhase::countdown) return;
    if (retreat_eligibility(rules, state, state.player, frame, gate) != RetreatRefusal::none) {
        state = {}; return;
    }
    if (frame < state.deadline) return;
    std::erase_if(state.actors, [&](const auto id) {
        const auto actor = std::find_if(actors.begin(), actors.end(), [&](const auto& row) { return row.id == id; });
        return actor == actors.end() || !eligible(*actor, state.player) || actor->engines_disabled;
    });
    state.phase = RetreatPhase::escaping;
    state.cinematic_duration = 5 * logical_frames_per_second; // WR-52; the owner extends this for a station
}
bool complete_retreat_escape(RetreatState& state) {
    if (state.phase != RetreatPhase::escaping) return false;
    state.phase = RetreatPhase::escaped; return true;
}
core::Result<RetreatAttrition> retreat_attrition(const std::span<const RetreatActor> actors,
    const PlayerId player, const math::Fixed factor, const std::span<const RetreatProtection> protection,
    const std::function<std::uint32_t()>& draw15) {
    std::map<std::uint32_t, math::Fixed> categories;
    for (const auto& source : protection) {
        const auto [at, inserted] = categories.emplace(source.category, source.value);
        if (!inserted) at->second = std::max(at->second, source.value);
    }
    math::Fixed mitigation;
    for (const auto& [category, value] : categories) {
        (void)category;
        const auto added = math::add(mitigation, value);
        if (!added) return core::Result<RetreatAttrition>::failure(added.error());
        mitigation = added.value();
    }
    const auto threshold = math::subtract(factor, mitigation);
    if (!threshold) return core::Result<RetreatAttrition>::failure(threshold.error());
    if (!draw15) return fail<RetreatAttrition>("retreat attrition needs the synchronized draw provider");
    RetreatAttrition out;
    for (auto at = actors.rbegin(); at != actors.rend(); ++at) {
        if (!at->live || at->owner != player || !at->persistent || at->team_member || at->carries_named_hero) continue;
        const auto draw = draw15(); ++out.draws;
        if (draw > 32767) return fail<RetreatAttrition>("retreat draw exceeds synchronized range");
        const auto value = math::Fixed::from_ratio(draw, 32767).value();
        if (value < threshold.value()) out.destroyed.push_back(at->id);
    }
    return core::Result<RetreatAttrition>::success(std::move(out));
}
std::vector<std::uint8_t> encode_retreat_state(const RetreatState& state) {
    std::vector<std::uint8_t> out;
    put(out, 3, 4); put(out, static_cast<std::uint8_t>(state.phase), 1); put(out, state.forced, 1);
    put(out, state.station_wait, 1); put(out, state.station_destroyed, 1); put(out, state.cinematic_frame, 4);
    put(out, state.playback_reported, 1); put(out, state.playback_playing, 1); put(out, state.cinematic_duration, 4);
    put(out, state.player, 4); put(out, state.deadline, 8); put(out, state.actors.size(), 4);
    for (const auto id : state.actors) put(out, id, 8);
    return out;
}
core::Result<RetreatState> decode_retreat_state(const std::span<const std::uint8_t> bytes) {
    std::size_t cursor{};
    const auto read = [&](const unsigned size) -> std::optional<std::uint64_t> {
        if (size > bytes.size() - cursor) return {};
        std::uint64_t value{};
        for (unsigned i = 0; i < size; ++i) value |= static_cast<std::uint64_t>(bytes[cursor++]) << (i * 8U);
        return value;
    };
    const auto version = read(4), phase = read(1), forced = read(1), wait = read(1), station = read(1), cinematic = read(4);
    const auto reported = read(1), playing = read(1), duration = read(4);
    const auto player = read(4), deadline = read(8), count = read(4);
    if (!version || *version != 3 || !phase || *phase > 3 || !forced || *forced > 1
        || !wait || *wait > 1 || !station || *station > 1 || !cinematic
        || !reported || *reported > 1 || !playing || *playing > 1 || !duration
        || !player || !deadline || !count || *count > 1000000 || *count > (bytes.size() - cursor) / 8)
        return fail<RetreatState>("malformed retreat state");
    RetreatState out; out.phase = static_cast<RetreatPhase>(*phase); out.forced = *forced != 0;
    out.player = static_cast<PlayerId>(*player); out.deadline = *deadline;
    out.station_wait = *wait != 0; out.station_destroyed = *station != 0; out.cinematic_frame = static_cast<std::uint32_t>(*cinematic);
    out.playback_reported = *reported != 0; out.playback_playing = *playing != 0;
    out.cinematic_duration = static_cast<std::uint32_t>(*duration);
    std::set<EntityId> ids;
    for (std::uint64_t i = 0; i < *count; ++i) {
        const auto id = read(8);
        if (!id || !*id || !ids.insert(*id).second) return fail<RetreatState>("invalid retreat actor identity");
        out.actors.push_back(*id);
    }
    if (cursor != bytes.size() || (out.phase == RetreatPhase::idle
        ? out.player || out.deadline || out.forced || !out.actors.empty() || out.station_wait
            || out.station_destroyed || out.cinematic_frame || out.playback_reported || out.playback_playing
            || out.cinematic_duration : !out.player || !out.deadline)
        || (out.station_destroyed && !out.station_wait)
        || (out.playback_playing && !out.playback_reported)
        || (out.cinematic_duration && out.cinematic_duration != 5 * logical_frames_per_second
            && out.cinematic_duration != 15 * logical_frames_per_second)
        || ((out.phase == RetreatPhase::escaping || out.phase == RetreatPhase::escaped)
            && (!out.cinematic_duration || out.cinematic_frame > out.cinematic_duration))
        || (out.station_wait && out.cinematic_frame < 90)
        || (out.station_destroyed && out.cinematic_frame < 181)
        || (out.phase == RetreatPhase::countdown && (out.station_wait || out.station_destroyed || out.cinematic_frame
            || out.playback_reported || out.playback_playing || out.cinematic_duration)))
        return fail<RetreatState>("inconsistent retreat state");
    return core::Result<RetreatState>::success(std::move(out));
}
} // namespace eawr::sim::tactical
