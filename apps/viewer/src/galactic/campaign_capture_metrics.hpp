#pragma once

#include "eawr/platform/live_game_session.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace eawr::viewer::galactic {
// Capture-only reader of immutable public facts. Counts committed departures,
// completed production/battles and unique captures beyond each player's start.
class CampaignCaptureMetrics final {
public:
    struct Player {
        std::int64_t credits_raw{}, tech{}, initial_tech{}, allowed_tech{};
        std::uint64_t planets_owned{}, builds_started{}, builds_completed{}, units_built_space{}, units_built_ground{};
        std::uint64_t fleet_moves{}, attacks{}, battles{}, battles_won{}, battles_lost{};
        std::uint64_t last_activity_frame{}, maximum_idle_frames{};
        std::optional<std::uint64_t> first_production_frame{};
        std::set<std::uint64_t> start_planets{}, captured_beyond_start{};
        void activity(std::uint64_t frame) {
            if (frame < last_activity_frame) return;
            maximum_idle_frames = std::max(maximum_idle_frames, frame - last_activity_frame);
            last_activity_frame = frame;
        }
    };
    std::uint64_t cursor{}, observed_session_tick{};
    std::string failure, end_state_hash;

    void observe(const platform::LiveGameFrame& publication, const platform::LiveGalacticEvents& feed) {
        if (!publication.latest) return;
        const auto& snapshot = *publication.latest;
        const auto frame = snapshot.clock().galactic_frame;
        observed_session_tick = publication.session_tick;
        if (feed.incomplete) { failure = "AI proof event retention gap"; return; }
        // A bounded driven request keeps the feed and snapshot at the same boundary.
        if (!initialized_) {
            for (const auto& player : snapshot.players()) players_.try_emplace(player.player_id);
            for (const auto& planet : snapshot.planets()) players_[planet.owner].start_planets.insert(planet.id);
            update_accounts(snapshot, true);
            initialized_ = true;
            sample(snapshot);
        }
        for (const auto& seat : snapshot.seats()) {
            seats_[seat.seat_id.value] = seat.player;
            active_ai_seen_ |= seat.active && seat.kind == sim::SeatKind::ai;
            if (seat.active && seat.kind == sim::SeatKind::human) ++human_seat_observations_;
        }
        for (const auto& fleet : snapshot.fleets()) fleet_owners_[fleet.id] = fleet.owner;
        if (snapshot.production()) for (const auto& planet : snapshot.production()->planets)
            for (std::size_t channel = 0; channel < planet.queues.size(); ++channel)
                for (const auto& entry : planet.queues[channel]) {
                    if (builds_.contains(entry.build_id)) continue;
                    std::optional<sim::galactic::ProductionKind> kind;
                    for (const auto& offer : planet.offers) if (offer.type == entry.type) kind = offer.kind;
                    builds_.emplace(entry.build_id, std::make_tuple(entry.builder, channel, kind));
                }
        for (const auto& row : feed.events) {
            if (row.event.tick > snapshot.completed_tick()) { failure = "AI proof feed is ahead of its publication"; return; }
            const auto& event = row.event;
            using Kind = sim::galactic::GalacticEventKind;
            if (event.kind == Kind::movement_begin) {
                const auto owner = fleet_owners_.find(event.subject);
                if (owner == fleet_owners_.end()) { failure = "AI proof departure owner unavailable"; return; }
                auto& player = players_[owner->second]; ++player.fleet_moves; player.activity(frame);
            } else if (event.kind == Kind::production_started || event.kind == Kind::production_completed) {
                auto build = builds_.find(event.sequence);
                if (build == builds_.end()) {
                    // Zero-duration purchases may complete in their admission tick.
                    const auto seat = seats_.find(event.producer.value);
                    if (seat != seats_.end()) {
                        auto purchase = std::make_tuple(seat->second, std::size_t{}, std::optional<sim::galactic::ProductionKind>{});
                        if (snapshot.production()) for (const auto& planet : snapshot.production()->planets)
                            if (planet.planet == event.subject) for (const auto& offer : planet.offers)
                                if (offer.type == event.value) purchase = {seat->second, static_cast<std::size_t>(offer.domain), offer.kind};
                        builds_.emplace(event.sequence, purchase);
                    }
                    build = builds_.find(event.sequence);
                }
                if (build == builds_.end()) { failure = "AI proof production owner unavailable"; return; }
                const auto& [owner, channel, kind] = build->second;
                auto& player = players_[owner]; player.activity(frame);
                if (event.kind == Kind::production_started) {
                    ++player.builds_started;
                    if (!player.first_production_frame) player.first_production_frame = frame;
                } else {
                    ++player.builds_completed;
                    // Keep the first published admission classification across offer changes.
                    if (!kind) ++unclassified_builds_[owner];
                    else if (*kind == sim::galactic::ProductionKind::unit) {
                        if (channel == 1) ++player.units_built_space;
                        else ++player.units_built_ground;
                    }
                }
            } else if (event.kind == Kind::planet_owner_changed) {
                auto& player = players_[static_cast<sim::galactic::PlayerId>(event.value)]; player.activity(frame);
                if (!player.start_planets.contains(event.subject)) player.captured_beyond_start.insert(event.subject);
            } else if (event.kind == Kind::conflict_pending && row.pending_alert && attacked_.insert(event.subject).second) {
                auto& player = players_[row.pending_alert->invader]; ++player.attacks; player.activity(frame);
                players_[row.pending_alert->defender].activity(frame);
            }
        }
        cursor = feed.through;
        const auto completed = [&](const campaign::BattleResult& result, auto invader, auto defender) {
            if (!completed_battles_.insert(result.conflict).second) return;
            for (const auto id : {invader, defender}) {
                auto& player = players_[id]; ++player.battles; player.activity(frame);
                if (result.winner == id) ++player.battles_won; else ++player.battles_lost;
            }
        };
        if (publication.auto_resolve_outcome) {
            const auto& outcome = *publication.auto_resolve_outcome;
            completed(outcome.result, outcome.invader, outcome.defender);
        }
        if (publication.result) {
            const auto conflict = publication.result_conflict ? publication.result_conflict : publication.conflict;
            if (!conflict) { failure = "AI proof completed battle participants unavailable"; return; }
            completed(*publication.result, conflict->invader, conflict->defender);
        }
        update_accounts(snapshot, false);
        if (frame && frame % (60 * sim::galactic::galactic_frames_per_second) == 0) sample(snapshot);
    }

    void sample(const sim::galactic::GalacticSnapshot& snapshot) {
        const auto frame = snapshot.clock().galactic_frame;
        if (!samples_.empty() && last_sample_ == frame) return;
        for (auto& [id, player] : players_) {
            player.planets_owned = static_cast<std::uint64_t>(std::count_if(snapshot.planets().begin(), snapshot.planets().end(),
                [id](const auto& planet) { return planet.owner == id; }));
        }
        std::ostringstream output;
        output << "{\"galactic_frame\":" << frame << ",\"players\":[";
        bool first = true;
        for (const auto& [id, p] : players_) {
            if (!first) output << ',';
            first = false;
            output << "{\"player\":" << id << ",\"credits_raw\":" << p.credits_raw << ",\"planets_owned\":" << p.planets_owned
                << ",\"starting_planets\":" << p.start_planets.size() << ",\"planets_taken_beyond_start\":" << p.captured_beyond_start.size()
                << ",\"builds_started\":" << p.builds_started << ",\"builds_completed\":" << p.builds_completed
                << ",\"units_built_space\":" << p.units_built_space << ",\"units_built_ground\":" << p.units_built_ground
                << ",\"builds_unclassified\":" << unclassified_builds_[id]
                << ",\"units_built_available\":" << (unclassified_builds_[id] == 0 ? "true" : "false")
                << ",\"fleet_moves\":" << p.fleet_moves << ",\"attacks\":" << p.attacks << ",\"battles\":" << p.battles
                << ",\"battles_won\":" << p.battles_won << ",\"battles_lost\":" << p.battles_lost
                << ",\"initial_tech\":" << p.initial_tech << ",\"allowed_tech\":" << p.allowed_tech << ",\"tech\":" << p.tech
                << ",\"maximum_idle_frames\":" << std::max(p.maximum_idle_frames, frame - p.last_activity_frame)
                << ",\"last_activity_frame\":" << p.last_activity_frame << ",\"first_production_frame\":";
            if (p.first_production_frame) output << *p.first_production_frame; else output << "null";
            output << ",\"plans_executed\":[],\"plans_available\":false,\"plans_unavailable_reason\":\"plan observer not published\"}";
        }
        output << "]}";
        samples_.push_back(output.str()); last_sample_ = frame;
    }

    [[nodiscard]] std::string json() const {
        std::ostringstream output;
        output << "{\"all_ai\":" << (active_ai_seen_ && human_seat_observations_ == 0 ? "true" : "false")
            << ",\"event_feed_complete\":" << (failure.empty() ? "true" : "false")
            << ",\"frame_time_available\":false,\"end_state_hash\":\"" << end_state_hash << "\",\"samples\":[";
        for (std::size_t i = 0; i < samples_.size(); ++i) output << (i ? "," : "") << samples_[i];
        output << "]}"; return output.str();
    }
private:
    void update_accounts(const sim::galactic::GalacticSnapshot& snapshot, bool initial) {
        if (!snapshot.player_economy()) return;
        for (const auto& account : snapshot.player_economy()->players) {
            auto& player = players_[account.player];
            if (initial) { player.initial_tech = account.current_tech; player.allowed_tech = account.maximum_tech; }
            else if (account.current_tech > player.tech) player.activity(snapshot.clock().galactic_frame);
            player.tech = account.current_tech; player.credits_raw = account.credits.raw();
        }
    }
    bool initialized_{}, active_ai_seen_{};
    std::uint64_t human_seat_observations_{}, last_sample_{};
    std::map<sim::galactic::PlayerId, Player> players_;
    std::map<std::uint32_t, sim::galactic::PlayerId> seats_;
    std::map<std::uint64_t, sim::galactic::PlayerId> fleet_owners_;
    std::map<std::uint64_t, std::tuple<sim::galactic::PlayerId, std::size_t, std::optional<sim::galactic::ProductionKind>>> builds_;
    std::map<sim::galactic::PlayerId, std::uint64_t> unclassified_builds_;
    std::set<std::uint64_t> attacked_, completed_battles_;
    std::vector<std::string> samples_;
};
} // namespace eawr::viewer::galactic
