#pragma once

// Quality-run driver for the async galactic AI comparison (tools/gc_quality, tools/gc_quality/README.md).
//
// One seed of one arm: the installed campaign stepped headless for N ticks with the native AI on, the human seat
// answering its prompts with the ordinary auto-resolve policy, and a compact decision log written as NDJSON. The log
// holds only what an outside observer sees (committed galactic events with the owner and target class resolved at the
// time, periodic per-player samples, the goal reader's summary lines), so the same comparison reads any future arm
// without touching the simulation. Test-only: nothing here feeds the simulation, and the run reads production data
// through the public live-session API only.
//
// The log goes to standard output as "GCQ <json>" lines (a pool job returns only its test log) and, when a
// directory is given, to <directory>/decision-log.ndjson. Schema "gcq-1" is documented in tools/gc_quality/README.md.

#include "eawr/core/diagnostic.hpp"
#include "eawr/core/result.hpp"
#include "eawr/platform/live_game_session.hpp"
#include "eawr/sim/galactic/events.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galactic_quality_run {
namespace gc = eawr::sim::galactic;

// The asynchronous arm needs the A2.3 batch driver. Until it exists the "async" arm runs the reference schedule and the
// log says so (stub=true), which keeps the whole pipeline testable as reference against reference. Flip this to true,
// and select K=15, D=30 in run(), in the slice that lands the driver.
inline constexpr bool async_arm_available = false;

struct Options {
    std::filesystem::path directory; // empty: standard output only
    std::uint64_t ticks{54000};
    std::uint64_t seed{1};
    std::string arm{"reference"};   // reference | async
    std::string probe{"none"};      // none | near1 | nearall | deep1: a scripted human fleet sent at an AI planet
    std::uint64_t probe_tick{3000};
    std::uint64_t sample_period{300};
    std::uint64_t goal_period{900};
    std::string profile{"stock"};   // label only: stock | raw | bfc
    std::string campaign;
    std::size_t workers{1};
};

namespace detail {
template<class T> T take(eawr::core::Result<T> result) {
    if (!result) throw std::runtime_error(result.error().code + " " + result.error().message);
    return std::move(result).value();
}

[[nodiscard]] inline std::string quote(std::string_view text) {
    std::string out{"\""};
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { out += ' '; }
        else out += static_cast<char>(c);
    }
    out += '"';
    return out;
}

// Minimal JSON object writer: keys are literals, values numbers, booleans or escaped strings.
class Json {
public:
    Json& u(std::string_view key, std::uint64_t value) { return raw(key, std::to_string(value)); }
    Json& i(std::string_view key, std::int64_t value) { return raw(key, std::to_string(value)); }
    Json& d(std::string_view key, double value) { return raw(key, std::to_string(value)); }
    Json& b(std::string_view key, bool value) { return raw(key, value ? "true" : "false"); }
    Json& s(std::string_view key, std::string_view value) { return raw(key, quote(value)); }
    Json& raw(std::string_view key, std::string_view value) {
        text_ += first_ ? "{" : ",";
        first_ = false;
        text_ += quote(key);
        text_ += ':';
        text_ += value;
        return *this;
    }
    [[nodiscard]] std::string done() const { return first_ ? "{}" : text_ + "}"; }
private:
    std::string text_;
    bool first_{true};
};

class Log {
public:
    explicit Log(const std::filesystem::path& directory) {
        if (directory.empty()) return;
        std::filesystem::create_directories(directory);
        file_.open(directory / "decision-log.ndjson", std::ios::trunc);
    }
    void line(const Json& row) {
        const auto text = row.done();
        std::cout << "GCQ " << text << '\n';
        if (file_.is_open()) file_ << text << '\n';
    }
    void flush() {
        std::cout.flush();
        if (file_.is_open()) file_.flush();
    }
private:
    std::ofstream file_;
};

struct FleetInfo {
    gc::PlayerId owner{};
    gc::PlanetId orbit{};
    std::size_t members{};
    bool transit{};
};
using FleetMap = std::unordered_map<std::uint64_t, FleetInfo>;
using OwnerMap = std::unordered_map<std::uint64_t, gc::PlayerId>;

inline void read_galaxy(const gc::GalacticSnapshot& snapshot, FleetMap& fleets, OwnerMap& planets) {
    fleets.clear();
    planets.clear();
    for (const auto& planet : snapshot.planets()) planets[planet.id] = planet.owner;
    for (const auto& fleet : snapshot.fleets())
        fleets[fleet.id] = FleetInfo{fleet.owner, fleet.orbit, fleet.members.size(), fleet.motion.transit.has_value()};
}

// Breadth-first hop path over the route graph; empty when unreachable.
[[nodiscard]] inline std::vector<gc::PlanetId> hop_path(const std::unordered_map<std::uint64_t, std::vector<std::uint64_t>>& graph,
    gc::PlanetId from, gc::PlanetId to) {
    if (from == to) return {from};
    std::unordered_map<std::uint64_t, std::uint64_t> parent;
    std::deque<std::uint64_t> queue{from};
    parent[from] = from;
    while (!queue.empty()) {
        const auto at = queue.front();
        queue.pop_front();
        const auto edges = graph.find(at);
        if (edges == graph.end()) continue;
        for (const auto next : edges->second) {
            if (parent.contains(next)) continue;
            parent[next] = at;
            if (next == to) {
                std::vector<gc::PlanetId> path{to};
                for (auto step = to; step != from;) { step = parent.at(step); path.push_back(step); }
                std::reverse(path.begin(), path.end());
                return path;
            }
            queue.push_back(next);
        }
    }
    return {};
}
} // namespace detail

// Fixture: the cherry-picked InstalledLive of ai_campaign_stock_tests.cpp (files, campaign, setup, human, ai_players,
// prepare(install, periods, observe)).
template<class Fixture, class Periods>
int run(const Fixture& fixture, const Options& options, const Periods& service_periods) {
    using Live = eawr::platform::LiveGameSession;
    using detail::Json;
    using detail::take;
    if (!options.ticks) throw std::runtime_error("quality run requires nonzero ticks");
    const bool stub = options.arm == "async" && !async_arm_available;
    if (options.arm != "reference" && options.arm != "async") throw std::runtime_error("quality run arm must be reference or async");
    if (options.probe != "none" && options.probe != "near1" && options.probe != "nearall" && options.probe != "deep1")
        throw std::runtime_error("quality run probe must be none, near1, nearall or deep1");
    detail::Log sink(options.directory);
    const auto started = std::chrono::steady_clock::now();

    auto prepared = fixture.prepare(true, service_periods, true);
    const auto goal_reader = prepared->native_ai_goal_reader();
    std::map<gc::PlayerId, std::string> faction_of;
    for (const auto& row : fixture.setup.players) faction_of[row.player.player_id] = row.faction;
    {
        std::string players{"["};
        bool first = true;
        for (const auto& ai : prepared->native_ai_players()) {
            players += (first ? "" : ",");
            first = false;
            Json row;
            row.u("p", ai.player).s("type", ai.player_type).s("faction", faction_of[ai.player]);
            players += row.done();
        }
        players += ']';
        Json meta;
        meta.s("r", "meta").s("schema", "gcq-1").u("seed", options.seed).s("arm", options.arm)
            .s("arm_effective", stub || options.arm == "reference" ? "reference" : "async").b("stub", stub)
            .s("profile", options.profile).s("campaign", fixture.campaign).u("ticks", options.ticks)
            .s("probe", options.probe).u("probe_tick", options.probe_tick).u("human", fixture.human.value)
            .u("workers", options.workers).u("sample_period", options.sample_period).raw("ai", players);
        sink.line(meta);
        sink.flush();
    }

    Live::Options live_options;
    live_options.workers = options.workers;
    live_options.pacing = Live::Pacing::driven;
    auto live = take(Live::start(std::move(prepared), live_options));

    const auto human_player = static_cast<gc::PlayerId>(fixture.human.value);
    const auto is_ai = [&](gc::PlayerId player) { return fixture.ai_players.contains(player); };

    // Unattended human policy: ordinary auto-resolve and summary acknowledgement, the same answers the tick profile gives.
    const auto answer_prompts = [&] {
        const auto frame = live->frame();
        if (frame.summary_hold) {
            live->submit(eawr::platform::LiveGameOrder{fixture.human, gc::BattleSummaryAcknowledge{frame.summary_hold}, {}});
        } else if (frame.pending_conflict && frame.pending_conflict->choice_ready) {
            const auto choice = eawr::platform::battle_choice_order(frame, fixture.human, frame.pending_conflict->conflict.id, false);
            if (choice) live->submit(*choice);
        }
    };
    const auto advance = [&](std::uint64_t tick) {
        answer_prompts();
        live->advance_to(tick);
        if (!live->wait_for(tick, std::chrono::minutes(30))) {
            const auto error = live->failure();
            throw std::runtime_error(error ? eawr::core::format_diagnostic(*error) : "quality run frame timed out");
        }
    };

    // Galactic technology account per player (current_tech / maximum_tech of GalacticPlayerEconomy, never a tactical value):
    // one "tech" row at the first tick and at every change.
    std::map<gc::PlayerId, std::pair<std::int64_t, std::int64_t>> tech_seen;
    const auto log_tech = [&](std::uint64_t tick, const gc::GalacticSnapshot& snapshot) {
        const auto& economy = snapshot.player_economy();
        if (!economy) return;
        for (const auto& account : economy->players) {
            if (!is_ai(account.player) && account.player != human_player) continue;
            const std::pair<std::int64_t, std::int64_t> now{account.current_tech, account.maximum_tech};
            const auto seen = tech_seen.find(account.player);
            if (seen != tech_seen.end() && seen->second == now) continue;
            tech_seen[account.player] = now;
            Json out;
            out.s("r", "tech").u("t", tick).u("p", account.player).i("tech", now.first).i("mx", now.second);
            sink.line(out);
        }
    };

    // Tech-steal trace (test-only): authored type names by id, so production rows can name their type, and the droid-team
    // hero actor (the company the Rebel advance-tech plan asks for) followed through the live actor records: a "drd" row at
    // tick 1 and at every change of owner, planet, container, fleet, transit or life, plus "abil" rows for the ability results.
    std::unordered_map<std::uint64_t, std::string> type_names;
    std::set<std::uint64_t> droid_types;
    for (const auto& binding : fixture.setup.types) {
        type_names[binding.id] = binding.original_type;
        std::string upper = binding.original_type;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper == "DROIDS_TEAM") droid_types.insert(binding.id);
    }
    struct DroidState {
        gc::PlayerId owner{}, planet_owner{};
        gc::PlanetId planet{}, at{};
        std::uint64_t container{}, fleet{};
        bool transit{}, dead{}, deleting{};
        bool operator==(const DroidState&) const = default;
    };
    std::map<std::uint64_t, DroidState> droids_seen;
    std::set<std::uint64_t> droid_ids; // actors of the droid-team type right now (event filter)
    std::set<std::uint64_t> abilities_seen;
    detail::FleetMap fleets_prev, fleets_now;
    detail::OwnerMap planets_prev, planets_now;
    const auto log_droids = [&](std::uint64_t tick, const gc::GalacticSnapshot& snapshot) {
        if (droid_types.empty()) return;
        std::map<std::uint64_t, DroidState> now;
        for (const auto& actor : snapshot.actor_records()) {
            if (!droid_types.contains(actor.original_type) && !droid_types.contains(actor.current_type)) continue;
            DroidState state;
            state.owner = actor.owner; state.planet = actor.planet; state.container = actor.container;
            state.dead = actor.dead; state.deleting = actor.delete_pending;
            if (const auto at = planets_now.find(actor.planet); at != planets_now.end()) state.planet_owner = at->second;
            now[actor.id] = state;
        }
        droid_ids.clear();
        for (const auto& [id, state] : now) { static_cast<void>(state); droid_ids.insert(id); }
        if (!now.empty() || !droids_seen.empty())
            for (const auto& fleet : snapshot.fleets())
                for (const auto& member : fleet.members)
                    if (const auto at = now.find(member.id); at != now.end()) {
                        at->second.fleet = fleet.id; at->second.at = fleet.orbit; at->second.transit = fleet.motion.transit.has_value();
                    }
        for (const auto& [id, state] : now) {
            const auto seen = droids_seen.find(id);
            if (seen != droids_seen.end() && seen->second == state) continue;
            droids_seen[id] = state;
            Json out;
            out.s("r", "drd").u("t", tick).u("id", id).u("o", state.owner).u("pl", state.planet).u("po", state.planet_owner)
                .u("ct", state.container).u("fl", state.fleet).u("at", state.at).b("tr", state.transit).b("dead", state.dead)
                .b("del", state.deleting);
            sink.line(out);
        }
        for (auto seen = droids_seen.begin(); seen != droids_seen.end();) {
            if (now.contains(seen->first)) { ++seen; continue; }
            Json out;
            out.s("r", "drd").u("t", tick).u("id", seen->first).b("gone", true);
            sink.line(out);
            seen = droids_seen.erase(seen);
        }
    };
    const auto log_abilities = [&](std::uint64_t tick, const gc::GalacticSnapshot& snapshot) {
        for (const auto& result : snapshot.ability_results()) {
            if (!abilities_seen.insert(result.request).second) continue;
            Json out;
            out.s("r", "abil").u("t", tick).u("req", result.request).u("actor", result.actor).u("pl", result.planet)
                .u("o", result.player).u("out", static_cast<std::uint64_t>(result.outcome))
                .u("k", static_cast<std::uint64_t>(result.kind)).u("item", result.item).u("tech", result.technology);
            sink.line(out);
        }
    };
    advance(1);
    detail::read_galaxy(*live->frame().latest, fleets_now, planets_now);
    log_tech(1, *live->frame().latest);
    log_droids(1, *live->frame().latest);
    std::uint64_t cursor{};
    std::uint64_t rejected_total{}, orders_total{};
    bool probe_done = options.probe == "none";

    const auto issue_probe = [&](std::uint64_t tick) {
        probe_done = true;
        Json row;
        row.s("r", "probe").u("t", tick).s("scenario", options.probe);
        const auto snapshot = live->frame().latest;
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> graph;
        for (const auto& route : snapshot->routes()) { graph[route.from].push_back(route.to); graph[route.to].push_back(route.from); }
        for (auto& [planet, edges] : graph) { static_cast<void>(planet); std::sort(edges.begin(), edges.end()); }
        // Idle human fleets, strongest first (ties: lowest id).
        std::vector<const gc::Fleet*> idle;
        for (const auto& fleet : snapshot->fleets())
            if (fleet.owner == human_player && !fleet.motion.transit && !fleet.members.empty()) idle.push_back(&fleet);
        std::stable_sort(idle.begin(), idle.end(), [](const auto* a, const auto* b) { return a->members.size() > b->members.size(); });
        if (idle.empty()) { row.b("issued", false).s("reason", "no idle human fleet"); sink.line(row); return; }
        // Candidate targets: AI-owned planets reachable by hop count. The first idle fleet (strongest first) that can
        // reach any AI planet leads; the others only follow in the "nearall" scenario.
        std::size_t ai_planets{};
        for (const auto& planet : snapshot->planets()) if (is_ai(planet.owner)) ++ai_planets;
        std::optional<std::pair<std::size_t, gc::PlanetId>> best;
        for (std::size_t lead_index = 0; lead_index < idle.size() && !best; ++lead_index) {
            for (const auto& planet : snapshot->planets()) {
                if (!is_ai(planet.owner)) continue;
                const auto path = detail::hop_path(graph, idle[lead_index]->orbit, planet.id);
                if (path.size() < 2) continue;
                const auto hops = path.size() - 1;
                const bool better = !best || (options.probe == "deep1" ? hops > best->first : hops < best->first);
                if (better) best = std::pair{hops, planet.id};
            }
            if (best && lead_index) std::swap(idle[0], idle[lead_index]);
        }
        if (!best) {
            row.b("issued", false).s("reason", "no reachable AI planet").u("idle_fleets", idle.size())
                .u("ai_planets", ai_planets).u("routes", snapshot->routes().size()).u("graph_planets", graph.size());
            sink.line(row);
            return;
        }
        const std::size_t count = options.probe == "nearall" ? idle.size() : 1;
        std::string ids{"["};
        std::size_t sent{};
        for (std::size_t index = 0; index < count; ++index) {
            const auto path = detail::hop_path(graph, idle[index]->orbit, best->second);
            if (path.size() < 2) continue;
            gc::MoveFleet move;
            move.fleet = idle[index]->id;
            move.path = path;
            live->submit(eawr::platform::LiveGameOrder{fixture.human, move, {}});
            ids += (sent++ ? "," : "") + std::to_string(idle[index]->id);
        }
        ids += ']';
        row.b("issued", sent > 0).u("target", best->second).u("target_owner", planets_now.at(best->second))
            .u("hops", best->first).u("fleets", sent).raw("fleet_ids", ids);
        sink.line(row);
    };

    // The cumulative refusal counters of the goal reader, kept as the last seen summary lines. With `detail`, the per-goal
    // lines of tech goals (goal function or plan name mentions "tech") are logged as "tgoal" rows: which goal exists, its
    // plan and whether a plan is running (tech-up measurement, tools/gc_quality/techup.py).
    const auto goal_lines = [&](std::uint64_t tick, bool summaries, bool detail) {
        if (!goal_reader) return;
        for (const auto& line : goal_reader()) {
            // Summary lines only ("player N type: ..."); per-goal detail lines carry " goal=" and are not logged.
            if (line.find(" goal=") != std::string::npos && line.find("definition=") != std::string::npos) {
                if (!detail) continue;
                std::string lower = line;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (lower.find("tech") == std::string::npos) continue;
                Json row;
                row.s("r", "tgoal").u("t", tick).s("line", line);
                sink.line(row);
                continue;
            }
            if (!summaries) continue;
            Json row;
            row.s("r", "goals").u("t", tick).s("line", line);
            sink.line(row);
        }
    };

    static const std::set<gc::GalacticEventKind> logged{
        gc::GalacticEventKind::movement_begin, gc::GalacticEventKind::movement_finished,
        gc::GalacticEventKind::production_started, gc::GalacticEventKind::production_completed,
        gc::GalacticEventKind::fleet_destroyed, gc::GalacticEventKind::planet_owner_changed,
        gc::GalacticEventKind::conflict_pending, gc::GalacticEventKind::planet_combat_begin,
        gc::GalacticEventKind::auto_resolve_won, gc::GalacticEventKind::auto_resolve_lost,
        gc::GalacticEventKind::command_rejected,
        // Tech-steal trace: landings, ability preparation/settlement and the droid-team actor joining or leaving a fleet.
        gc::GalacticEventKind::land_conflict_requested, gc::GalacticEventKind::formation_land,
        gc::GalacticEventKind::ability_prepared, gc::GalacticEventKind::ability_settled, gc::GalacticEventKind::ability_cancelled,
        gc::GalacticEventKind::movement_canceled, gc::GalacticEventKind::movement_bounced,
        gc::GalacticEventKind::member_attached, gc::GalacticEventKind::member_detached};

    for (std::uint64_t index = 1; index < options.ticks; ++index) {
        const auto tick = live->completed_tick() + 1;
        if (!probe_done && tick >= options.probe_tick) issue_probe(tick);
        advance(tick);
        const auto frame = live->frame();
        fleets_prev.swap(fleets_now);
        planets_prev.swap(planets_now);
        detail::read_galaxy(*frame.latest, fleets_now, planets_now);
        log_tech(tick, *frame.latest);
        log_droids(tick, *frame.latest);
        log_abilities(tick, *frame.latest);
        const auto fleet_of = [&](std::uint64_t id) -> const detail::FleetInfo* {
            if (const auto at = fleets_now.find(id); at != fleets_now.end()) return &at->second;
            if (const auto at = fleets_prev.find(id); at != fleets_prev.end()) return &at->second;
            return nullptr;
        };
        const auto planet_owner = [&](std::uint64_t id) -> gc::PlayerId {
            if (const auto at = planets_now.find(id); at != planets_now.end()) return at->second;
            if (const auto at = planets_prev.find(id); at != planets_prev.end()) return at->second;
            return 0;
        };
        const auto rows = live->galactic_events_after(cursor);
        if (rows.incomplete) throw std::runtime_error("quality run event cursor fell behind");
        cursor = rows.through;
        for (const auto& row : rows.events) {
            const auto& event = row.event;
            if (!logged.contains(event.kind)) continue;
            if ((event.kind == gc::GalacticEventKind::member_attached || event.kind == gc::GalacticEventKind::member_detached)
                && !droid_ids.contains(event.subject)) continue;
            Json out;
            out.s("r", "ev").u("t", event.tick).s("k", gc::to_string(event.kind));
            switch (event.kind) {
            case gc::GalacticEventKind::movement_begin: {
                const auto* fleet = fleet_of(event.subject);
                const gc::PlayerId owner = fleet ? fleet->owner : 0;
                const auto target_owner = planet_owner(event.value);
                const char* target_class = target_owner == 0 ? "neutral" : (target_owner == owner ? "own" : "enemy");
                out.u("o", owner).u("s", event.subject).u("from", fleet ? fleet->orbit : 0).u("to", event.value)
                    .s("tc", target_class).u("n", fleet ? fleet->members : 0);
                if (is_ai(owner)) ++orders_total;
                break;
            }
            case gc::GalacticEventKind::movement_finished: {
                const auto* fleet = fleet_of(event.subject);
                out.u("o", fleet ? fleet->owner : 0).u("s", event.subject).u("at", event.value).u("po", planet_owner(event.value));
                break;
            }
            case gc::GalacticEventKind::production_started:
            case gc::GalacticEventKind::production_completed:
                out.u("o", planet_owner(event.subject)).u("s", event.subject).u("ty", event.value).u("sq", event.sequence);
                if (const auto name = type_names.find(event.value); name != type_names.end()) out.s("tn", name->second);
                if (event.kind == gc::GalacticEventKind::production_started && is_ai(planet_owner(event.subject))) ++orders_total;
                break;
            case gc::GalacticEventKind::fleet_destroyed: {
                const auto* fleet = fleet_of(event.subject);
                out.u("o", fleet ? fleet->owner : 0).u("s", event.subject).u("at", fleet ? fleet->orbit : 0);
                break;
            }
            case gc::GalacticEventKind::planet_owner_changed:
                out.u("s", event.subject).u("v", event.value).u("prev", row.previous_owner.value_or(0));
                break;
            case gc::GalacticEventKind::command_rejected: {
                gc::PlayerId owner = 0;
                for (const auto& seat : frame.latest->seats()) if (seat.seat_id == event.producer) owner = seat.player;
                out.u("o", owner).u("seat", event.producer.value).s("why", gc::to_string(event.reason));
                if (is_ai(owner)) ++rejected_total;
                break;
            }
            default:
                out.u("s", event.subject).u("v", event.value);
                if (row.pending_alert) out.u("inv", row.pending_alert->invader).u("def", row.pending_alert->defender);
                break;
            }
            sink.line(out);
        }
        if (tick % options.sample_period == 0 && frame.latest->player_economy()) {
            std::map<gc::PlayerId, std::pair<std::size_t, std::size_t>> holdings; // planets, fleets
            for (const auto& [planet, owner] : planets_now) { static_cast<void>(planet); ++holdings[owner].first; }
            for (const auto& [fleet, info] : fleets_now) { static_cast<void>(fleet); ++holdings[info.owner].second; }
            for (const auto& account : frame.latest->player_economy()->players) {
                if (!is_ai(account.player) && account.player != human_player) continue;
                Json out;
                out.s("r", "smp").u("t", tick).u("p", account.player).i("cr", account.credits.trunc_to_integer())
                    .i("tech", account.current_tech).i("mx", account.maximum_tech).u("pl", holdings[account.player].first).u("fl", holdings[account.player].second);
                sink.line(out);
            }
        }
        if (tick % options.goal_period == 0 || tick % options.sample_period == 0)
            goal_lines(tick, tick % options.goal_period == 0, tick % options.sample_period == 0);
        if (tick % 1800 == 0) sink.flush();
    }
    goal_lines(live->completed_tick(), true, true);
    live->stop();
    const auto hashes = live->tick_hashes();
    std::uint64_t digest = 1469598103934665603ULL;
    for (std::size_t tick = 0; tick < hashes.size(); ++tick) {
        for (const unsigned char c : hashes[tick]) digest = (digest ^ c) * 1099511628211ULL;
        if ((tick + 1) % 900 == 0) {
            Json out;
            out.s("r", "hash").u("t", tick + 1).s("h", hashes[tick]);
            sink.line(out);
        }
    }
    Json end;
    end.s("r", "end").u("ticks", live->completed_tick()).u("hashes", hashes.size()).u("hash_fnv", digest)
        .u("ai_orders", orders_total).u("ai_rejected", rejected_total)
        .d("wall_s", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
    sink.line(end);
    sink.flush();
    return 0;
}
} // namespace galactic_quality_run
