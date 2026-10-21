#pragma once

#include "galaxy_view.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <span>
#include <utility>

namespace eawr::viewer::galactic {

// References into the current publication; storage is reused between frames.
template<class T> class CorruptionIndex final {
public:
    void clear() { values_.clear(); }
    void emplace(ObjectId id, const T* value) { values_.emplace_back(id, value); }
    void sort() { std::sort(values_.begin(), values_.end(), [](const auto& a, const auto& b) { return a.first < b.first; }); }
    [[nodiscard]] auto find(ObjectId id) const {
        const auto it = std::lower_bound(values_.begin(), values_.end(), id, [](const auto& pair, auto key) { return pair.first < key; });
        return it != values_.end() && it->first == id ? it : values_.end();
    }
    [[nodiscard]] auto begin() const { return values_.begin(); }
    [[nodiscard]] auto end() const { return values_.end(); }
private:
    std::vector<std::pair<ObjectId, const T*>> values_;
};

// WGP-19..22: values supplied by the corruption/action owner after observer
// admission. Absence is unavailable, never an invitation to inspect hidden state.
// WGP-47..49 supply parameter mapping and connection geometry.
struct CorruptionPlanetFact final {
    ObjectId planet{};
    bool corrupted{}, transitioning{}, increasing{};
    std::string particle_type;
    std::optional<float> brightness, scale, energy;
    float fraction{1.0F};
    bool source_nonempty{}, target_nonempty{};
};
struct CorruptionParameters final {
    std::array<float, 2> brightness{1, 1}, scale{1, 1}, energy{1, 1};
};
[[nodiscard]] inline float corruption_fraction(const sim::galactic::corruption_fixture::Transition& state,
    std::uint64_t frame) {
    if (!state.end || state.start == state.end) return 1;
    if (state.end < state.start) return 0;
    if (frame <= state.start) return 0;
    if (frame >= state.end) return 1;
    return static_cast<float>(static_cast<double>(frame - state.start) / static_cast<double>(state.end - state.start));
}
inline void configure_corruption(CorruptionPlanetFact& fact, const CorruptionParameters& parameters) {
    const auto interpolate = [&](const auto& values) {
        const float a = values[fact.source_nonempty ? 1U : 0U];
        return a + (values[fact.target_nonempty ? 1U : 0U] - a) * fact.fraction;
    };
    fact.brightness = interpolate(parameters.brightness);
    fact.scale = interpolate(parameters.scale);
    fact.energy = interpolate(parameters.energy);
}
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
struct CorruptionObservationScratch final {
    CorruptionIndex<sim::galactic::ObserverPlanet> sources;
    CorruptionIndex<sim::galactic::LiveCorruptionTransition> transitions;
};
// Conservative project admission while U-06's observer/type/level mapping is
// unresolved: only discovered records for the displayed local observer qualify.
// This is a read-only projection of already published inputs, not discovery logic.
inline void observed_corruption(
    const sim::galactic::GalacticSnapshot& snapshot, const GalaxyView& view,
    const std::string& particle_type, CorruptionPublication& result, CorruptionObservationScratch& scratch) {
    result.seat = view.local.seat; result.planets.clear(); result.effects.clear();
    if (!snapshot.observer() || snapshot.view_player() != view.local.player) return;
    auto& sources = scratch.sources; sources.clear();
    for (const auto& planet : snapshot.observer()->planets) sources.emplace(planet.planet, &planet);
    sources.sort();
    auto& transitions = scratch.transitions; transitions.clear();
    for (const auto& transition : snapshot.corruption_transitions()) transitions.emplace(transition.planet, &transition);
    transitions.sort();
    for (const auto& observed : snapshot.observations()) {
        if (observed.player != view.local.player || !observed.discovered) continue;
        const auto source = sources.find(observed.planet);
        if (source == sources.end()) continue;
        CorruptionPlanetFact fact;
        fact.planet = observed.planet; fact.corrupted = source->second->corrupted; fact.particle_type = particle_type;
        fact.source_nonempty = fact.target_nonempty = fact.corrupted;
        const auto transition = transitions.find(fact.planet);
        if (transition != transitions.end()) {
            const auto& state = transition->second->transition;
            fact.transitioning = state.active(); fact.increasing = state.increasing;
            fact.source_nonempty = state.current != sim::galactic::corruption_fixture::Type::none;
            fact.target_nonempty = fact.transitioning
                ? state.target != sim::galactic::corruption_fixture::Type::none : fact.source_nonempty;
            fact.fraction = fact.transitioning ? corruption_fraction(state, snapshot.clock().galactic_frame) : 1.0F;
        }
        result.planets.push_back(std::move(fact));
    }
}
[[nodiscard]] inline CorruptionPublication observed_corruption(
    const sim::galactic::GalacticSnapshot& snapshot, const GalaxyView& view, const std::string& particle_type) {
    CorruptionPublication result; CorruptionObservationScratch scratch;
    observed_corruption(snapshot, view, particle_type, result, scratch);
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
    std::uint64_t generation{};
    friend bool operator==(const CorruptionConnection&, const CorruptionConnection&) = default;
};
// WGP-48/49: validated GameConstants; stock values are not runtime defaults.
struct CorruptionStyle final {
    std::string line_particle;
    float radius{}, endpoint_offset{}, grow_seconds{}, path_width{};
    std::array<float, 4> path_colour{};
    Vec3 path_offset;
    std::string path_texture;
};
struct CorruptionLineGeometry final {
    Vec3 origin, x, y, z;
    float height{};
};
[[nodiscard]] inline std::optional<CorruptionLineGeometry> corruption_line_geometry(
    Vec3 a, Vec3 b, const CorruptionStyle& style) {
    const Vec3 delta{b.x-a.x, b.y-a.y, b.z-a.z};
    const float distance = std::sqrt(delta.x*delta.x+delta.y*delta.y+delta.z*delta.z);
    // U-06: degenerate/negative-height geometry is not runtime-qualified.
    if (!std::isfinite(distance) || distance <= 0 || style.radius < 0
        || distance < 2*style.endpoint_offset) return {};
    CorruptionLineGeometry result;
    result.z = {delta.x/distance, delta.y/distance, delta.z/distance};
    const auto z = result.z;
    const float planar = std::sqrt(z.x*z.x+z.y*z.y);
    result.x = planar > 0 ? Vec3{-z.y/planar, z.x/planar, 0} : Vec3{1,0,0};
    const auto x = result.x;
    result.y = {z.y*x.z-z.z*x.y, z.z*x.x-z.x*x.z, z.x*x.y-z.y*x.x};
    result.origin = {a.x+z.x*style.endpoint_offset, a.y+z.y*style.endpoint_offset, a.z+z.z*style.endpoint_offset};
    result.height = distance-2*style.endpoint_offset;
    return result;
}
class CorruptionLineGrowth final {
public:
    [[nodiscard]] float service(std::uint64_t frame, float grow_seconds) {
        if (grow_seconds <= 0 || !std::isfinite(grow_seconds)) return submitted_;
        const float increment = 1/(grow_seconds*static_cast<float>(sim::galactic::galactic_frames_per_second));
        const bool first = next_ < 0;
        if (!first && frame == frame_) return submitted_;
        if (first || frame < frame_) next_ = 0;
        const auto updates = !first && frame > frame_ ? frame-frame_ : 1;
        if (updates > 1) {
            // Reconstruct skipped eligible updates without a catch-up loop. The
            // one overshooting increment is submitted before the next clamps.
            const float skipped = static_cast<float>(updates-1);
            const float until_end = next_ < 1 ? std::ceil((1-next_)/increment) : 0;
            next_ = skipped > until_end ? 1 : next_+skipped*increment;
        }
        submitted_ = next_;
        next_ = next_ >= 1 ? 1 : next_+increment;
        frame_ = frame;
        return submitted_;
    }
private:
    float next_{-1}, submitted_{};
    std::uint64_t frame_{};
};

[[nodiscard]] inline std::array<float, 4> corruption_path_colour(
    const CorruptionStyle& style, const CorruptionPlanetFact& owner) {
    const float factor = owner.transitioning ? (owner.source_nonempty ? 1 - owner.fraction : owner.fraction) : 1;
    std::array<float, 4> result{};
    for (std::size_t i = 0; i < 3; ++i) result[i] = std::trunc(std::clamp(style.path_colour[i] * factor, 0.0F, 255.0F)) / 255;
    result[3] = 1;
    return result;
}

class CorruptionVisuals final {
public:
    struct Work final { std::size_t route_visits{}, neighbor_visits{}, adjacency_rebuilds{}; };
    [[nodiscard]] Work work() const { return work_; }
    void update(const GalaxyView& view, const CorruptionPublication& publication) {
        work_ = {};
        attachments.clear(); connections.clear(); one_shots.clear();
        if (publication.seat != view.local.seat) { reset(); return; }
        if (seat_ != publication.seat || observer_ != view.local.player) {
            reset(); seat_ = publication.seat; observer_ = view.local.player;
        }
        auto& facts = facts_; facts.clear();
        for (const auto& fact : publication.planets) facts.emplace(fact.planet, &fact);
        facts.sort();
        present_.clear();
        for (const auto& planet : view.planets) present_.emplace(planet.id, &planet);
        present_.sort();
        std::erase_if(previous_, [&](const auto& pair) {
            const auto fact = facts.find(pair.first);
            return fact == facts.end() || (!fact->second->corrupted && !fact->second->transitioning)
                || fact->second->particle_type.empty() || present_.find(pair.first) == present_.end();
        });
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
            previous_.insert_or_assign(planet.id, attachment);
            attachments.push_back(std::move(attachment));
        }
        // WGP-20/21: one shared pair represents both reciprocal records. Retain
        // its original emitter owner/generation until either endpoint removes it.
        auto& adjacent = adjacent_; auto& neighbors = neighbors_;
        bool changed = routes_.size() != view.routes.size();
        std::size_t route_index{};
        for (const auto& route : view.routes) {
            ++work_.route_visits;
            if (!changed && routes_[route_index] != std::pair{route.from, route.to}) changed = true;
            ++route_index;
        }
        if (changed) {
            ++work_.adjacency_rebuilds;
            routes_.clear(); adjacent.clear(); neighbors.clear(); linked_.clear();
            for (const auto& route : view.routes) {
                routes_.emplace_back(route.from, route.to);
                if (route.from == route.to) continue;
                if (adjacent.emplace(std::min(route.from, route.to), std::max(route.from, route.to)).second) {
                    neighbors[route.from].push_back(route.to); neighbors[route.to].push_back(route.from);
                    linked_.try_emplace(route.from, false); linked_.try_emplace(route.to, false);
                }
            }
        }
        std::erase_if(lines_, [&](const auto& pair) {
            const auto a = facts.find(pair.second.from), b = facts.find(pair.second.to);
            return !adjacent.contains(pair.first) || a == facts.end() || b == facts.end()
                || !a->second->corrupted || !b->second->corrupted;
        });
        std::erase_if(paths_, [&](const auto& pair) {
            const auto a = facts.find(pair.second.from), b = facts.find(pair.second.to);
            return !adjacent.contains(pair.first) || a == facts.end() || b == facts.end() || !a->second->transitioning
                || (!a->second->corrupted && !a->second->increasing)
                || (!b->second->corrupted && !b->second->increasing);
        });
        for (auto& [id, linked] : linked_) { static_cast<void>(id); linked = false; }
        for (const auto& [key, line] : lines_) {
            static_cast<void>(key); linked_.at(line.from) = linked_.at(line.to) = true;
        }
        for (const auto& [id, source] : facts) {
            if (!source->corrupted && !source->increasing) continue;
            const auto around = neighbors.find(id);
            if (around == neighbors.end() || linked_.at(id)) continue;
            for (const auto other : around->second) {
                ++work_.neighbor_visits;
                const auto neighbor = facts.find(other);
                if (neighbor == facts.end()) continue;
                const std::pair<ObjectId, ObjectId> key{std::min(id, other), std::max(id, other)};
                if (!source->transitioning && neighbor->second->corrupted && !lines_.contains(key)) {
                    lines_.emplace(key, CorruptionConnection{id, other, false, ++generation_});
                    linked_.at(id) = linked_.at(other) = true;
                }
                if (source->transitioning && (neighbor->second->corrupted || neighbor->second->increasing)
                    && !paths_.contains(key)) paths_.emplace(key, CorruptionConnection{id, other, true, ++generation_});
            }
        }
        for (const auto& [key, line] : lines_) { static_cast<void>(key); connections.push_back(line); }
        for (const auto& [key, path] : paths_) { static_cast<void>(key); connections.push_back(path); }
        // Re-presenting the same publication cannot replay an explosion.
        auto& effects = effects_; effects.assign(publication.effects.begin(), publication.effects.end());
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
    void reset() {
        const auto generation = generation_; *this = {}; generation_ = generation;
    }
    std::vector<CorruptionAttachment> attachments;
    std::vector<CorruptionConnection> connections;
    std::vector<GalacticEffectFact> one_shots;
private:
    Work work_;
    CorruptionIndex<CorruptionPlanetFact> facts_;
    CorruptionIndex<PlanetView> present_;
    std::vector<std::pair<ObjectId, ObjectId>> routes_;
    std::set<std::pair<ObjectId, ObjectId>> adjacent_;
    std::map<ObjectId, std::vector<ObjectId>> neighbors_;
    std::map<ObjectId, bool> linked_;
    std::vector<GalacticEffectFact> effects_;
    std::map<ObjectId, CorruptionAttachment> previous_;
    std::map<std::pair<ObjectId, ObjectId>, CorruptionConnection> lines_, paths_;
    SeatId seat_{};
    PlayerId observer_{};
    std::uint64_t generation_{}, effect_cursor_{};
};

} // namespace eawr::viewer::galactic
