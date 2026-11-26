#pragma once

#include "fleet_traits.hpp"
#include "eawr/sim/galactic/fleet_intelligence.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// #8428: the inputs of the fleet HUD's observed-fleet projection besides the publication itself. `rank_key` names the
// answers of `icon_rank` and `hero_rank`: the caller changes it whenever either may answer differently for a type.
struct FleetProjectionInputs {
    sim::galactic::PlayerId local{};
    std::span<const sim::galactic::FleetDisplayType> traits;
    std::size_t capacity{};
    std::span<const sim::galactic::FleetDisplayVisibility> visibility;
    std::function<std::optional<std::uint32_t>(sim::galactic::TypeId)> icon_rank;
    std::span<const std::pair<sim::EntityId, std::int64_t>> indices;
    std::function<std::optional<std::int64_t>(sim::galactic::TypeId)> hero_rank;
    std::span<const sim::EntityId> departing; // sorted, as observed_fleets requires
    std::uint64_t rank_key{};
};

// The projection's cards, presence rows and presence planets; the work receipts are left out.
[[nodiscard]] inline bool same_fleet_projection(const sim::galactic::FleetIntelligence& a, const sim::galactic::FleetIntelligence& b) {
    return a.cards == b.cards && a.presence == b.presence && a.presence_planets == b.presence_planets;
}

// The planet of the first difference between two projections, for a verification report; nullopt when they agree.
[[nodiscard]] inline std::optional<sim::galactic::PlanetId> first_fleet_projection_difference(
    const sim::galactic::FleetIntelligence& a, const sim::galactic::FleetIntelligence& b) {
    for (std::size_t index = 0; index < std::min(a.cards.size(), b.cards.size()); ++index)
        if (!(a.cards[index] == b.cards[index])) return std::min(a.cards[index].planet, b.cards[index].planet);
    if (a.cards.size() != b.cards.size())
        return a.cards.size() > b.cards.size() ? a.cards[b.cards.size()].planet : b.cards[a.cards.size()].planet;
    for (std::size_t index = 0; index < std::min(a.presence.size(), b.presence.size()); ++index)
        if (!(a.presence[index] == b.presence[index])) return std::min(a.presence[index].planet, b.presence[index].planet);
    if (a.presence.size() != b.presence.size())
        return a.presence.size() > b.presence.size() ? a.presence[b.presence.size()].planet : b.presence[a.presence.size()].planet;
    if (a.presence_planets != b.presence_planets) return sim::galactic::PlanetId{};
    return std::nullopt;
}

// One projection of `snapshot`, in the order every fleet HUD reader sees: cards grouped by planet (ascending), each
// planet's cards in the projection's own fleet order. Local transport traits are applied, as the HUD always did.
[[nodiscard]] inline sim::galactic::FleetIntelligence project_observed_fleets(const sim::galactic::GalacticSnapshot& snapshot,
    const FleetProjectionInputs& inputs, std::span<const sim::galactic::FleetDisplayVisibility> visibility) {
    auto result = sim::galactic::observed_fleets(snapshot, inputs.local, inputs.traits, inputs.capacity, visibility,
        inputs.icon_rank, inputs.indices, inputs.hero_rank, inputs.departing);
    if (snapshot.fleet_control()) apply_local_transport_traits(result.cards, snapshot.fleets(), snapshot.fleet_control()->types);
    std::stable_sort(result.cards.begin(), result.cards.end(),
        [](const auto& a, const auto& b) { return a.planet < b.planet; });
    return result;
}
[[nodiscard]] inline sim::galactic::FleetIntelligence project_observed_fleets(const sim::galactic::GalacticSnapshot& snapshot,
    const FleetProjectionInputs& inputs) {
    return project_observed_fleets(snapshot, inputs, inputs.visibility);
}

// #8428: the observed-fleet projection of consecutive publications, reprojecting only the planets whose inputs changed.
// The projection has no cross-planet state: a card, presence row or presence planet of planet P reads P's local
// observation row and sight row, the fleets orbiting P (transit fleets never enter it) with their member trees
// (actor records, movement contents, fleet rows named by a member), and publication-wide tables (observer types and
// players, fleet-control types) plus the caller's inputs. A change to a publication-wide table or input reprojects
// everything; otherwise only planets whose own rows or member trees changed. Rows shared by storage are equal without a
// compare. `update` always equals project_observed_fleets of the same publication (the developer verify mode and the
// tests check that).
class IncrementalFleetProjection {
public:
    struct Receipts {
        std::uint64_t full{}, partial{}, reused{}, planets{};
    };

    const sim::galactic::FleetIntelligence& update(const std::shared_ptr<const sim::galactic::GalacticSnapshot>& snapshot,
        const FleetProjectionInputs& inputs) {
        last_full_ = false; last_changed_.clear();
        if (!snapshot) { reset(); return result_; }
        const auto& now = *snapshot;
        auto planets = planet_rows(now, inputs);
        const bool same_inputs = previous_ && same_global(now, inputs);
        if (!same_inputs) {
            result_ = project_observed_fleets(now, inputs);
            remember(snapshot, inputs, std::move(planets));
            last_full_ = true; ++receipts_.full; receipts_.planets += planets_.size();
            return result_;
        }
        // The same publication can still come with other sight rows: the per-planet diff below runs either way (on the
        // same publication every row is shared by storage, so it is pointer compares).
        const auto& before = *previous_;
        const auto changed_actors = changed_ids(before.actor_records(), now.actor_records(),
            [](const auto& row) { return row.id; });
        std::vector<sim::EntityId> changed_containers;
        if (before.movement() != now.movement())
            changed_containers = changed_ids(std::span(before.movement()->contents), std::span(now.movement()->contents),
                [](const auto& row) { return row.container; });
        const bool trees_changed = !changed_actors.empty() || !changed_containers.empty();
        // Merge the two sorted planet tables; a planet only one side holds is changed.
        std::size_t old_at = 0, new_at = 0;
        while (old_at < planets_.size() || new_at < planets.size()) {
            const bool take_old = new_at == planets.size() || (old_at < planets_.size() && planets_[old_at].planet < planets[new_at].planet);
            const bool take_new = old_at == planets_.size() || (new_at < planets.size() && planets[new_at].planet < planets_[old_at].planet);
            if (take_old) { last_changed_.push_back(planets_[old_at++].planet); continue; }
            if (take_new) { last_changed_.push_back(planets[new_at++].planet); continue; }
            const auto& old_row = planets_[old_at++];
            const auto& new_row = planets[new_at++];
            if (!same_planet(old_row, new_row)
                || (trees_changed && touches(now, before, new_row, changed_actors, changed_containers)))
                last_changed_.push_back(new_row.planet);
        }
        if (!last_changed_.empty()) {
            std::vector<sim::galactic::FleetDisplayVisibility> sight;
            for (const auto& row : inputs.visibility)
                if (std::binary_search(last_changed_.begin(), last_changed_.end(), row.planet)) sight.push_back(row);
            auto part = project_observed_fleets(now, inputs, sight);
            const auto dirty = [this](const sim::galactic::PlanetId planet) {
                return std::binary_search(last_changed_.begin(), last_changed_.end(), planet);
            };
            std::erase_if(result_.cards, [&](const auto& card) { return dirty(card.planet); });
            std::erase_if(result_.presence, [&](const auto& row) { return dirty(row.planet); });
            std::erase_if(result_.presence_planets, dirty);
            for (auto& card : part.cards) result_.cards.push_back(std::move(card));
            result_.presence.insert(result_.presence.end(), part.presence.begin(), part.presence.end());
            result_.presence_planets.insert(result_.presence_planets.end(), part.presence_planets.begin(), part.presence_planets.end());
            std::stable_sort(result_.cards.begin(), result_.cards.end(), [](const auto& a, const auto& b) { return a.planet < b.planet; });
            std::sort(result_.presence.begin(), result_.presence.end(),
                [](const auto& a, const auto& b) { return std::pair{a.planet, a.owner} < std::pair{b.planet, b.owner}; });
            std::sort(result_.presence_planets.begin(), result_.presence_planets.end());
            result_.observation_rows = part.observation_rows;
            result_.fleets_examined = part.fleets_examined;
            result_.members_examined = part.members_examined;
            ++receipts_.partial; receipts_.planets += last_changed_.size();
        } else {
            ++receipts_.reused;
        }
        remember(snapshot, inputs, std::move(planets));
        return result_;
    }

    [[nodiscard]] const sim::galactic::FleetIntelligence& result() const noexcept { return result_; }
    // The last update projected every planet.
    [[nodiscard]] bool last_full() const noexcept { return last_full_; }
    // The planets the last partial update reprojected, ascending; empty after a full or reused update.
    [[nodiscard]] std::span<const sim::galactic::PlanetId> last_changed() const noexcept { return last_changed_; }
    [[nodiscard]] const Receipts& receipts() const noexcept { return receipts_; }
    void reset() {
        previous_.reset(); result_ = {}; planets_.clear(); traits_.clear(); indices_.clear(); departing_.clear();
        last_full_ = false; last_changed_.clear();
    }

private:
    struct PlanetRows {
        sim::galactic::PlanetId planet{};
        const sim::galactic::ObservedPlanet* observation{};
        std::optional<std::optional<bool>> sight;
        std::vector<const sim::galactic::Fleet*> fleets;
    };

    // Per planet: the local player's first observation row, its first sight row and its orbit fleets in publication
    // order. Exactly the rows observed_fleets reads per planet (it keeps the first row of a repeated planet).
    static std::vector<PlanetRows> planet_rows(const sim::galactic::GalacticSnapshot& snapshot, const FleetProjectionInputs& inputs) {
        std::vector<PlanetRows> rows;
        const auto at = [&rows](const sim::galactic::PlanetId planet) -> PlanetRows& {
            const auto found = std::lower_bound(rows.begin(), rows.end(), planet,
                [](const auto& row, const auto key) { return row.planet < key; });
            if (found != rows.end() && found->planet == planet) return *found;
            return *rows.insert(found, PlanetRows{planet, nullptr, std::nullopt, {}});
        };
        for (const auto& row : snapshot.observations())
            if (row.player == inputs.local) { auto& slot = at(row.planet); if (!slot.observation) slot.observation = &row; }
        for (const auto& row : inputs.visibility) { auto& slot = at(row.planet); if (!slot.sight) slot.sight.emplace(row.current_visible); }
        for (const auto& fleet : snapshot.fleets())
            if (fleet.id && !fleet.motion.transit) at(fleet.orbit).fleets.push_back(&fleet);
        return rows;
    }

    static bool same_planet(const PlanetRows& before, const PlanetRows& now) {
        if (bool(before.observation) != bool(now.observation)) return false;
        if (before.observation && before.observation != now.observation && !(*before.observation == *now.observation)) return false;
        if (before.sight != now.sight || before.fleets.size() != now.fleets.size()) return false;
        for (std::size_t index = 0; index < now.fleets.size(); ++index)
            if (before.fleets[index] != now.fleets[index] && !(*before.fleets[index] == *now.fleets[index])) return false;
        return true;
    }

    // Ids whose rows differ between two id-sorted tables, including rows only one side holds; ascending.
    template<class Row, class Id>
    static std::vector<sim::EntityId> changed_ids(std::span<const Row> before, std::span<const Row> now, Id id) {
        std::vector<sim::EntityId> changed;
        if (before.data() == now.data() && before.size() == now.size()) return changed;
        std::size_t old_at = 0, new_at = 0;
        while (old_at < before.size() || new_at < now.size()) {
            if (new_at == now.size() || (old_at < before.size() && id(before[old_at]) < id(now[new_at]))) {
                changed.push_back(id(before[old_at++])); continue;
            }
            if (old_at == before.size() || id(now[new_at]) < id(before[old_at])) { changed.push_back(id(now[new_at++])); continue; }
            if (!(before[old_at] == now[new_at])) changed.push_back(id(now[new_at]));
            ++old_at; ++new_at;
        }
        std::sort(changed.begin(), changed.end());
        changed.erase(std::unique(changed.begin(), changed.end()), changed.end());
        return changed;
    }

    // Whether a changed actor record or contents row lies in the member tree of a fleet orbiting this planet, or a
    // member names a fleet row that changed (the combat-skip rule reads a fleet-role member's own fleet row).
    static bool touches(const sim::galactic::GalacticSnapshot& now, const sim::galactic::GalacticSnapshot& before,
        const PlanetRows& planet, const std::vector<sim::EntityId>& actors, const std::vector<sim::EntityId>& containers) {
        const auto* movement = now.movement();
        const auto changed = [&](const sim::EntityId id) {
            if (std::binary_search(actors.begin(), actors.end(), id) || std::binary_search(containers.begin(), containers.end(), id))
                return true;
            const auto fleet_row = [id](std::span<const sim::galactic::Fleet> fleets) -> const sim::galactic::Fleet* {
                const auto at = std::lower_bound(fleets.begin(), fleets.end(), id, [](const auto& row, const auto key) { return row.id < key; });
                return at != fleets.end() && at->id == id ? &*at : nullptr;
            };
            const auto* now_fleet = fleet_row(now.fleets());
            const auto* old_fleet = fleet_row(before.fleets());
            return bool(now_fleet) != bool(old_fleet) || (now_fleet && now_fleet != old_fleet && !(*now_fleet == *old_fleet));
        };
        const auto visit = [&](auto&& self, const sim::EntityId id, const std::size_t depth) -> bool {
            if (depth > 33 || changed(id)) return true;
            if (!movement) return false;
            const auto children = std::lower_bound(movement->contents.begin(), movement->contents.end(), id,
                [](const auto& row, const auto key) { return row.container < key; });
            if (children == movement->contents.end() || children->container != id) return false;
            for (const auto& child : children->children) if (self(self, child.id, depth + 1)) return true;
            return false;
        };
        for (const auto* fleet : planet.fleets) {
            if (changed(fleet->id)) return true;
            for (const auto& member : fleet->members) if (visit(visit, member.id, 0)) return true;
        }
        return false;
    }

    bool same_global(const sim::galactic::GalacticSnapshot& now, const FleetProjectionInputs& inputs) const {
        const auto& before = *previous_;
        if (inputs.local != local_ || inputs.capacity != capacity_ || inputs.rank_key != rank_key_
            || now.view_player() != before.view_player()) return false;
        if (!std::equal(inputs.traits.begin(), inputs.traits.end(), traits_.begin(), traits_.end())
            || !std::equal(inputs.indices.begin(), inputs.indices.end(), indices_.begin(), indices_.end())
            || !std::equal(inputs.departing.begin(), inputs.departing.end(), departing_.begin(), departing_.end())) return false;
        const auto& observer_now = now.observer();
        const auto& observer_before = before.observer();
        if (bool(observer_now) != bool(observer_before)) return false;
        if (observer_now && &*observer_now != &*observer_before
            && (observer_now->types != observer_before->types || observer_now->players != observer_before->players)) return false;
        const auto& control_now = now.fleet_control();
        const auto& control_before = before.fleet_control();
        if (bool(control_now) != bool(control_before)) return false;
        if (control_now && &*control_now != &*control_before && control_now->types != control_before->types) return false;
        // Contents rows are diffed per container; a publication with or without a movement table is a different shape.
        return bool(now.movement()) == bool(before.movement());
    }

    void remember(const std::shared_ptr<const sim::galactic::GalacticSnapshot>& snapshot, const FleetProjectionInputs& inputs,
        std::vector<PlanetRows> planets) {
        previous_ = snapshot; planets_ = std::move(planets);
        local_ = inputs.local; capacity_ = inputs.capacity; rank_key_ = inputs.rank_key;
        traits_.assign(inputs.traits.begin(), inputs.traits.end());
        indices_.assign(inputs.indices.begin(), inputs.indices.end());
        departing_.assign(inputs.departing.begin(), inputs.departing.end());
    }

    // The previous publication stays alive while the planet rows point into it.
    std::shared_ptr<const sim::galactic::GalacticSnapshot> previous_;
    sim::galactic::FleetIntelligence result_;
    std::vector<PlanetRows> planets_;
    sim::galactic::PlayerId local_{};
    std::size_t capacity_{};
    std::uint64_t rank_key_{};
    std::vector<sim::galactic::FleetDisplayType> traits_;
    std::vector<std::pair<sim::EntityId, std::int64_t>> indices_;
    std::vector<sim::EntityId> departing_;
    bool last_full_{};
    std::vector<sim::galactic::PlanetId> last_changed_;
    Receipts receipts_;
};

} // namespace eawr::viewer::galactic
