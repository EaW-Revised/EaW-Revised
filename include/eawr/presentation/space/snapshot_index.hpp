#pragma once

#include "eawr/presentation/space/fog_field.hpp"
#include "eawr/sim/tactical/snapshot.hpp"
#include "eawr/platform/live_session.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <map>
#include <utility>
#include <vector>

namespace eawr::presentation::space {

// Immutable snapshots already order instances by entity. Lookups use that index without
// allocating a second map. Keep the snapshot alive for every pointer returned from it.
[[nodiscard]] inline const sim::tactical::TacticalInstance* find_instance(
    const sim::tactical::TacticalSnapshot& snapshot, const sim::EntityId entity,
    std::uint64_t* comparisons = nullptr) noexcept {
    const auto instances = snapshot.instances();
    const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
        [comparisons](const sim::tactical::TacticalInstance& instance, const sim::EntityId id) {
            if (comparisons) ++*comparisons;
            return instance.entity_id < id;
        });
    return found != instances.end() && found->entity_id == entity ? &*found : nullptr;
}

// The frame, picking, sound and fog share the newest tick's visibility, liveness and sensors.
// Fractional presentation frames reuse them; a new snapshot (even of the same tick) or viewer
// invalidates them. Entity IDs may be sparse, so storage depends on live rows, never the largest ID.
class SnapshotIndex final {
public:
    struct Work final {
        std::uint64_t refreshes{};
        std::uint64_t instance_rows{};
    };

    void refresh(std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot,
                 const sim::tactical::PlayerId viewer) {
        if (snapshot == snapshot_ && viewer == viewer_) return;
        snapshot_ = std::move(snapshot);
        viewer_ = viewer;
        visible_.clear();
        alive_.clear();
        revealers_.clear();
        teams_.clear();
        observer_team_.reset();
        ++work_.refreshes;
        if (!snapshot_) return;
        const auto players = snapshot_->players();
        const auto seat = std::find_if(players.begin(), players.end(), [viewer](const auto& player) {
            return player.player_id == viewer;
        });
        const auto bit = seat == players.end() ? std::uint64_t{0}
            : std::uint64_t{1} << static_cast<std::size_t>(seat - players.begin());
        observer_team_ = seat == players.end() ? std::nullopt : std::optional{seat->team_id};
        for (const auto& player : players) teams_.emplace(player.player_id, player.team_id);
        alive_.reserve(snapshot_->instances().size());
        visible_.reserve(snapshot_->instances().size());
        const auto real = [](const sim::math::Fixed value) {
            return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
        };
        for (const auto& instance : snapshot_->instances()) {
            ++work_.instance_rows;
            alive_.push_back(instance.entity_id);
            if ((instance.visible_to & bit) != 0U) visible_.push_back(instance.entity_id);
            if (seat != players.end() && instance.team == seat->team_id && instance.reveal_range) {
                revealers_.push_back({real(instance.fixed_transform.rows[0][3]), real(instance.fixed_transform.rows[1][3]),
                                     real(*instance.reveal_range)});
            }
        }
    }

    [[nodiscard]] const sim::tactical::TacticalInstance* instance(const sim::EntityId entity) const noexcept {
        return snapshot_ ? find_instance(*snapshot_, entity) : nullptr;
    }
    [[nodiscard]] const std::vector<sim::EntityId>& visible() const noexcept { return visible_; }
    [[nodiscard]] const std::vector<sim::EntityId>& alive() const noexcept { return alive_; }
    [[nodiscard]] const std::vector<FogFieldRevealer>& revealers() const noexcept { return revealers_; }
    [[nodiscard]] const Work& work() const noexcept { return work_; }

    // V-04/16, WPJ-38: reuse the observer's immutable fog rows and alliance
    // inputs. A projectile outlives its shooter; no entity lookup or new scan
    // of units is needed. The caller supplies object stealth/force exceptions.
    [[nodiscard]] bool point_visible(const sim::tactical::PlayerId owner, const sim::math::Vec3& position,
                                    const platform::LiveFog* fog, const bool revealed = false,
                                    const bool enemy_stealth = false) const noexcept {
        if (revealed) return true;
        if (!observer_team_) return false;
        const auto team = teams_.find(owner);
        if (team != teams_.end() && team->second == *observer_team_) return true;
        if (enemy_stealth) return false;
        if (!fog) return true; // WPJ-38: fog disabled
        const auto& rules = fog->rules;
        const auto cell = rules.cell_size.raw();
        if (cell <= 0 || position.x < rules.map_left || position.y > rules.map_top) return false;
        // Unsigned differences also cover the full fixed-point range safely.
        const auto column = (static_cast<std::uint64_t>(position.x.raw())
            - static_cast<std::uint64_t>(rules.map_left.raw())) / static_cast<std::uint64_t>(cell);
        const auto row = (static_cast<std::uint64_t>(rules.map_top.raw())
            - static_cast<std::uint64_t>(position.y.raw())) / static_cast<std::uint64_t>(cell);
        if (column >= rules.cells_wide || row >= rules.cells_tall || row >= fog->values.size()) return false;
        const auto& values = fog->values[static_cast<std::size_t>(row)];
        return values && column < values->size() && (*values)[static_cast<std::size_t>(column)] != 0;
    }

private:
    std::shared_ptr<const sim::tactical::TacticalSnapshot> snapshot_;
    sim::tactical::PlayerId viewer_{};
    std::vector<sim::EntityId> visible_;
    std::vector<sim::EntityId> alive_;
    std::vector<FogFieldRevealer> revealers_;
    Work work_;
    std::optional<sim::tactical::TeamId> observer_team_;
    std::map<sim::tactical::PlayerId, sim::tactical::TeamId> teams_;
};

} // namespace eawr::presentation::space
