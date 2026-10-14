#pragma once

#include "galaxy_view.hpp"

#include <algorithm>
#include <map>
#include <span>
#include <utility>

namespace eawr::viewer::galactic {

// WGP-19..22: values supplied by the corruption/action owner after observer
// admission. Absence is unavailable, never an invitation to inspect hidden state.
// U-06 keeps level mapping, line geometry and action placement explicit.
struct CorruptionPlanetFact final {
    ObjectId planet{};
    bool corrupted{}, transitioning{}, increasing{};
    std::string particle_type;
    std::optional<float> brightness, scale, energy;
};
enum class GalacticEffectKind { sabotage, planet_destruction, superweapon_hack };
struct GalacticEffectFact final {
    std::uint64_t sequence{}; // Monotonic within this presentation stream.
    ObjectId planet{};
    GalacticEffectKind kind{};
    std::string particle_type; // Validated authored effect type, not a model guess.
    std::optional<Vec3> position;
    std::optional<std::size_t> layer;
    std::optional<float> lifetime_seconds;
};
struct CorruptionPublication final {
    SeatId seat{};
    std::vector<CorruptionPlanetFact> planets;
    std::vector<GalacticEffectFact> effects;
};
// Conservative project admission while U-06's observer/type/level mapping is
// unresolved: only discovered records for the displayed local observer qualify.
// This is a read-only projection of already published inputs, not discovery logic.
[[nodiscard]] inline CorruptionPublication observed_corruption(
    const sim::galactic::GalacticSnapshot& snapshot, const GalaxyView& view,
    const std::string& particle_type) {
    CorruptionPublication result; result.seat = view.local.seat;
    if (!snapshot.observer() || snapshot.observer()->local_player != view.local.player) return result;
    for (const auto& observed : snapshot.observations()) {
        if (observed.player != view.local.player || !observed.discovered) continue;
        const auto source = std::find_if(snapshot.observer()->planets.begin(), snapshot.observer()->planets.end(),
            [&](const auto& planet) { return planet.planet == observed.planet; });
        if (source == snapshot.observer()->planets.end()) continue;
        CorruptionPlanetFact fact;
        fact.planet = observed.planet; fact.corrupted = source->corrupted; fact.particle_type = particle_type;
        result.planets.push_back(std::move(fact));
    }
    return result;
}
struct CorruptionAttachment final {
    CorruptionPlanetFact fact;
    Vec3 position;
    std::uint64_t generation{};
};
struct CorruptionConnection final {
    ObjectId from{}, to{};
    bool path{};
    friend bool operator==(const CorruptionConnection&, const CorruptionConnection&) = default;
};
// Descriptor values come from validated GameConstants. U-06 does not yet
// establish how radius/offset/growth configure the particle or the path.
struct CorruptionStyle final {
    std::string line_particle;
    float radius{}, endpoint_offset{}, grow_seconds{}, path_width{};
    std::array<float, 4> path_colour{};
    Vec3 path_offset;
};

class CorruptionVisuals final {
public:
    void update(const GalaxyView& view, const CorruptionPublication& publication) {
        attachments.clear(); connections.clear(); one_shots.clear();
        if (publication.seat != view.local.seat) { reset(); return; }
        if (seat_ != publication.seat) { reset(); seat_ = publication.seat; }
        std::map<ObjectId, const CorruptionPlanetFact*> facts;
        for (const auto& fact : publication.planets) facts.emplace(fact.planet, &fact);
        std::map<ObjectId, CorruptionAttachment> next;
        for (const auto& planet : view.planets) {
            const auto found = facts.find(planet.id);
            if (found == facts.end()) continue;
            const auto& fact = *found->second;
            if ((!fact.corrupted && !fact.transitioning) || fact.particle_type.empty()) continue;
            const auto old = previous_.find(planet.id);
            const auto generation = old != previous_.end()
                && old->second.fact.particle_type == fact.particle_type
                ? old->second.generation : ++generation_;
            CorruptionAttachment attachment{fact, planet.position, generation};
            next.emplace(planet.id, attachment);
            attachments.push_back(std::move(attachment));
        }
        previous_ = std::move(next);
        // WGP-20/21: settled corrupted pairs carry lines; paths survive only
        // with an active transition. Reciprocal routes do not duplicate a pair.
        std::map<std::pair<ObjectId, ObjectId>, std::pair<bool, bool>> pairs;
        for (const auto& route : view.routes) {
            if (route.from == route.to) continue;
            const auto a = facts.find(route.from), b = facts.find(route.to);
            if (a == facts.end() || b == facts.end()) continue;
            const auto& x = *a->second; const auto& y = *b->second;
            const bool line = x.corrupted && y.corrupted && (!x.transitioning || !y.transitioning);
            const bool path = (x.transitioning && (x.corrupted || x.increasing) && (y.corrupted || y.increasing))
                || (y.transitioning && (y.corrupted || y.increasing) && (x.corrupted || x.increasing));
            if (!line && !path) continue;
            const auto key = std::minmax(route.from, route.to);
            pairs.emplace(key, std::pair{line, path});
        }
        for (const auto& [key, kinds] : pairs) {
            if (kinds.first) connections.push_back({key.first, key.second, false});
            if (kinds.second) connections.push_back({key.first, key.second, true});
        }
        // Re-presenting the same publication cannot replay an explosion.
        auto effects = publication.effects;
        std::stable_sort(effects.begin(), effects.end(), [](const auto& a, const auto& b) {
            return a.sequence < b.sequence;
        });
        for (const auto& effect : effects) {
            if (effect.sequence <= effect_cursor_) continue;
            effect_cursor_ = effect.sequence;
            if (std::none_of(view.planets.begin(), view.planets.end(), [&](const auto& p) {
                return p.id == effect.planet;
            })) continue;
            one_shots.push_back(effect);
        }
    }
    // A new campaign/observer stream owns a fresh cursor and attachment set.
    void reset() { *this = {}; }
    std::vector<CorruptionAttachment> attachments;
    std::vector<CorruptionConnection> connections;
    std::vector<GalacticEffectFact> one_shots;
private:
    std::map<ObjectId, CorruptionAttachment> previous_;
    SeatId seat_{};
    std::uint64_t generation_{}, effect_cursor_{};
};

} // namespace eawr::viewer::galactic
