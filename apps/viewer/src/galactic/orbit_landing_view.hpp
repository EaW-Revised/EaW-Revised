#pragma once

#include "eawr/platform/live_game_session.hpp"
#include "eawr/sim/galactic/control.hpp"

#include <algorithm>
#include <map>
#include <vector>

namespace eawr::viewer::galactic {
// GGS-12: remember the requested fleet while its land conflict is pending or active.
// This is presentation state only. A loaded mid-prompt save has no request event to reconstruct it.
class OrbitLandingView {
public:
    [[nodiscard]] const std::vector<sim::EntityId>& observe(const platform::LiveGameFrame& frame,
        std::span<const platform::LiveGalacticEvent> retained = {}) {
        const auto* conflict = frame.pending_conflict ? &frame.pending_conflict->conflict : frame.conflict.get();
        if (!frame.latest || !conflict || conflict->layer != campaign::BattleLayer::land) {
            requests_.clear();
            hidden_.clear();
            conflict_ = 0;
            publication_.reset();
            return hidden_;
        }
        if (conflict_ != conflict->id) requests_.clear();
        const auto& snapshot = *frame.latest;
        std::erase_if(requests_, [&](const auto& row) { return row.second != conflict->planet; });
        bool changed = false;
        const auto request = [&](const auto& event) {
            if (event.kind == sim::galactic::GalacticEventKind::land_conflict_requested && event.value == conflict->planet)
                changed = requests_.try_emplace(event.subject, event.value).second || changed;
        };
        for (const auto& row : retained) request(row.event);
        for (const auto& event : snapshot.events()) request(event);
        if (!changed && publication_ == frame.latest && conflict_ == conflict->id) return hidden_;
        conflict_ = conflict->id;
        publication_ = frame.latest;
        hidden_.clear();
        if (!snapshot.control() || !snapshot.fleet_control()) return hidden_;
        for (const auto& fleet : snapshot.fleets()) {
            if (!requests_.contains(fleet.id) || fleet.owner != conflict->invader || fleet.orbit != conflict->planet) continue;
            for (const auto& member : fleet.members) {
                const auto& types = snapshot.fleet_control()->types;
                const auto type = std::find_if(types.begin(), types.end(), [&](const auto& row) { return row.type == member.type; });
                if (type == types.end() || !type->transport) continue;
                const auto capable = sim::galactic::has_ground_capability(*snapshot.control(), member, snapshot.movement());
                if (capable && capable.value()) hidden_.push_back(member.id);
            }
        }
        std::sort(hidden_.begin(), hidden_.end());
        return hidden_;
    }
private:
    std::map<sim::EntityId, sim::galactic::PlanetId> requests_;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> publication_;
    sim::EntityId conflict_{};
    std::vector<sim::EntityId> hidden_;
};
} // namespace eawr::viewer::galactic
