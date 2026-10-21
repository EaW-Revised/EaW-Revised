#pragma once

// Opt-in qualification recorder. Owned and used only by the simulation thread;
// it observes the ordinary lockstep path and never edits the campaign state.
#include "eawr/net/galactic_adapter.hpp"
#include "eawr/sim/galactic/graph.hpp"
#include "eawr/sim/tactical/session.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>

namespace eawr::presentation::godot_backend {
class GalacticAcceptance {
    using Clock = std::chrono::steady_clock;
    using Payload = sim::galactic::CommandPayload;
    struct Battle {
        sim::EntityId id{};
        bool space{}, humans{};
        std::uint64_t begin{}, end{}, frames{};
    };
public:
    GalacticAcceptance(std::filesystem::path directory, std::string campaign, bool stock)
        : directory_(std::move(directory)), campaign_(std::move(campaign)), stock_(stock) {
        std::filesystem::create_directories(directory_);
        hashes_.open(directory_ / "hashes.csv"); costs_.open(directory_ / "costs.csv");
        trace_.open(directory_ / "trace.csv");
        hashes_ << "tick,sha256\n"; costs_ << "tick,sim_ms\n";
        trace_ << "event,seat,sequence,kind,tick,frame,epoch_us,steady_us\n";
    }
    static std::string kind(const Payload& value) {
        using namespace sim::galactic;
        if (std::holds_alternative<MoveFleet>(value)) return "move";
        if (std::holds_alternative<PauseGalaxy>(value)) return "pause";
        if (std::holds_alternative<ResumeGalaxy>(value)) return "resume";
        if (std::holds_alternative<SetSpeed>(value)) return "speed";
        if (std::holds_alternative<BattleChoice>(value)) return "battle_choice";
        if (std::holds_alternative<BattleSummaryAcknowledge>(value)) return "summary";
        return "other";
    }
    void event(std::string_view event, const net::SeatInput& input, std::uint64_t tick) {
        const auto decoded = net::decode_galactic_input(input);
        if (!decoded) { diagnostics_.push_back(decoded.error().code); return; }
        const auto epoch = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const auto steady = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
        trace_ << event << ',' << input.key.seat.value << ',' << input.key.sequence << ',' << kind(decoded.value().payload)
               << ',' << tick << ',' << tick << ',' << epoch << ',' << steady << '\n';
        if (event != "execute") pending_.insert_or_assign(input.key, input);
    }
    void initialize(const campaign::GameSession& game, sim::SeatId seat) {
        seat_ = seat; roster_.assign(game.galaxy().seats().begin(), game.galaxy().seats().end());
        native_ai_ = game.native_ai_installed();
    }
    template<class Submit>
    void script(const campaign::GameSession& game, sim::SeatId seat, std::uint64_t tick, Submit submit) {
        using namespace sim::galactic;
        const auto own = std::find_if(roster_.begin(), roster_.end(), [&](const auto& row) { return row.seat_id == seat; });
        if (own == roster_.end()) return;
        const auto offset = std::uint64_t{120} + seat.value * 120;
        if (tick >= offset && !paused_) { submit(PauseGalaxy{}); paused_ = true; }
        if (tick >= offset + 30 && !resumed_) { submit(ResumeGalaxy{}); resumed_ = true; }
        // The pause consumes a clock-change slot in the 45-second LAN window.
        // Wait for expiry before restoring 1x and requesting 2x (two slots).
        if (tick >= offset + 1400 && !baseline_speed_) {
            submit(SetSpeed{false, sim::math::Fixed::from_raw(sim::math::Fixed::scale)}); baseline_speed_ = true;
        }
        if (tick >= offset + 1430 && !speed_) {
            submit(SetSpeed{game.battle() != nullptr, sim::math::Fixed::from_raw(2 * sim::math::Fixed::scale)}); speed_ = true;
        }
        const auto* pending = game.pending_conflict();
        if (pending && (pending->invader == own->player || pending->defender == own->player)
            && choices_.insert(pending->id).second) submit(BattleChoice{pending->id, true});
        const auto* result_context = game.latest_tactical_context();
        if (const auto* result = game.latest_tactical_result(); result && result_context
            && (result_context->invader == own->player || result_context->defender == own->player)
            && summaries_.insert(result->conflict).second) submit(BattleSummaryAcknowledge{result->conflict});
        // A light ordinary movement every 30 session seconds. Read the data's graph,
        // ownership and fleets; never teleport, inject a conflict or assume campaign names.
        if (tick < 2400 || tick < next_move_ || game.battle() || pending) return;
        next_move_ = tick + 900;
        const auto snapshot = game.galaxy().snapshot();
        const auto* graph = game.galaxy().graph();
        if (!graph) return;
        // Each peer advances its own fleets towards an opposing human's planet.
        for (const auto& fleet : snapshot->fleets()) {
            if (fleet.owner != own->player || fleet.motion.transit || fleet.members.empty()) continue;
            std::map<PlanetId, PlanetId> parent{{fleet.orbit, fleet.orbit}};
            std::vector<PlanetId> queue{fleet.orbit};
            std::optional<PlanetId> destination;
            for (std::size_t i = 0; i < queue.size() && !destination; ++i) {
                const auto* node = graph->node(queue[i]); if (!node) continue;
                for (const auto& edge : node->edges) if (!parent.contains(edge.to)) {
                    parent.emplace(edge.to, queue[i]); queue.push_back(edge.to);
                    const auto planet = std::find_if(snapshot->planets().begin(), snapshot->planets().end(),
                        [&](const auto& row) { return row.id == edge.to; });
                    if (planet != snapshot->planets().end() && std::any_of(roster_.begin(), roster_.end(),
                        [&](const auto& row) { return row.kind == sim::SeatKind::human && row.player != own->player
                            && row.player == planet->owner; })) { destination = edge.to; break; }
                }
            }
            if (!destination) continue;
            std::vector<PlanetId> path{*destination};
            while (path.back() != fleet.orbit) path.push_back(parent.at(path.back()));
            std::reverse(path.begin(), path.end());
            submit(MoveFleet{fleet.id, std::move(path), {}});
            break;
        }
    }
    void observe(const campaign::GameSession& game, const campaign::GameTick& tick, double milliseconds) {
        last_tick_ = tick.completed_tick; last_hash_ = tick.state_sha256;
        hashes_ << last_tick_ << ',' << last_hash_ << '\n';
        costs_ << last_tick_ << ',' << std::setprecision(12) << milliseconds << '\n';
        for (auto at = pending_.begin(); at != pending_.end();) {
            if (at->first.tick >= last_tick_) { ++at; continue; }
            const auto decoded = net::decode_galactic_input(at->second);
            event("execute", at->second, last_tick_);
            if (decoded) {
                const auto name = kind(decoded.value().payload);
                if (name == "pause" || name == "resume" || name == "speed") {
                    const auto* speed = std::get_if<sim::galactic::SetSpeed>(&decoded.value().payload);
                    std::ostringstream row;
                    row << "{\"seat\":" << at->first.seat.value << ",\"tick\":" << last_tick_
                        << ",\"kind\":\"" << name << "\",\"factor_raw\":" << (speed ? speed->factor.raw() : 0) << '}';
                    clock_orders_.push_back(row.str());
                }
            }
            at = pending_.erase(at);
        }
        for (const auto& diagnostic : tick.diagnostics) diagnostics_.push_back(diagnostic.code);
        if (tick.galaxy && tick.galaxy != last_snapshot_) {
            for (const auto& event : tick.galaxy->events()) {
                using Kind = sim::galactic::GalacticEventKind;
                const auto name = event.kind == Kind::galaxy_paused ? "pause"
                    : event.kind == Kind::galaxy_resumed ? "resume"
                    : event.kind == Kind::speed_changed ? "speed" : nullptr;
                if (name) {
                    std::ostringstream row;
                    row << "{\"seat\":" << event.producer.value << ",\"tick\":" << last_tick_
                        << ",\"kind\":\"" << name << "\"}";
                    clock_changes_.push_back(row.str());
                }
                if (event.kind != sim::galactic::GalacticEventKind::movement_begin) continue;
                std::ostringstream row;
                row << "{\"seat\":" << event.producer.value << ",\"tick\":" << last_tick_
                    << ",\"fleet\":" << event.subject << '}';
                movements_.push_back(row.str());
            }
            last_snapshot_ = tick.galaxy;
        }
        const auto clock = game.galaxy().clock();
        game_frames_ += clock.galactic_frame - galaxy_frame_; galaxy_frame_ = clock.galactic_frame;
        if (game.battle() && game.conflict()) {
            const auto& conflict = *game.conflict();
            auto [at, inserted] = battles_.try_emplace(conflict.id);
            auto& battle = at->second;
            if (inserted) {
                battle.id = conflict.id; battle.begin = last_tick_; battle.space = conflict.layer == campaign::BattleLayer::space;
                const auto human = [&](auto player) { return std::any_of(roster_.begin(), roster_.end(),
                    [&](const auto& row) { return row.kind == sim::SeatKind::human && row.player == player; }); };
                battle.humans = human(conflict.invader) && human(conflict.defender);
            }
            const auto frame = game.battle()->completed_tick();
            game_frames_ += frame - battle.frames; battle.frames = frame;
        }
        if (tick.result) {
            const auto at = battles_.find(tick.result->conflict);
            if (at != battles_.end()) at->second.end = last_tick_;
            else auto_resolves_.push_back(tick.result->conflict);
        }
        if (native_ai_ && tick.mode == campaign::SessionMode::galaxy && clock.pause_depth == 0) ++ai_steps_;
    }
    void stall(bool waiting) { if (waiting && !waiting_) ++stalls_; waiting_ = waiting; }
    bool healthy() const { return hashes_ && costs_ && trace_; }
    void finish(bool agreed) {
        hashes_.flush(); costs_.flush(); trace_.flush();
        hashes_.close(); costs_.close(); trace_.close();
        std::ofstream output(directory_ / "gc.json");
        output << "{\"schema\":1,\"campaign\":" << std::quoted(campaign_) << ",\"lobby_mode\":\"galactic\","
               << "\"campaign_multiplayer\":true,\"stock_content\":" << (stock_ ? "true" : "false")
               << ",\"local_seat\":" << seat_.value << ",\"native_ai_installed\":" << (native_ai_ ? "true" : "false")
               << ",\"ai_steps\":" << ai_steps_ << ",\"input_delay\":" << delay
               << ",\"barrier_tick\":" << last_tick_ << ",\"agreed_end\":" << (agreed ? "true" : "false")
               << ",\"galactic_frames\":" << galaxy_frame_ << ",\"gameplay_frames\":" << game_frames_
               << ",\"final_state_sha256\":" << std::quoted(last_hash_) << ",\"desync\":false,\"stall_count\":" << stalls_
               << ",\"roster\":[";
        bool first = true;
        for (const auto& row : roster_) {
            if (row.kind == sim::SeatKind::script) continue;
            output << (first ? "" : ",") << "{\"seat\":" << row.seat_id.value << ",\"player\":" << row.player
                   << ",\"active\":" << (row.active ? "true" : "false")
                   << ",\"side\":" << row.human_side_id << ",\"human\":" << (row.kind == sim::SeatKind::human ? "true" : "false")
                   << ",\"control\":\"" << (row.kind == sim::SeatKind::human ? "human" : "ai") << "\"}"; first = false;
        }
        output << "],\"battles\":["; first = true;
        for (const auto& [id, battle] : battles_) {
            output << (first ? "" : ",") << "{\"conflict\":" << id << ",\"kind\":\"" << (battle.space ? "space" : "land")
                   << "\",\"human_vs_human\":" << (battle.humans ? "true" : "false") << ",\"begin_tick\":" << battle.begin
                   << ",\"return_tick\":" << battle.end << ",\"child_ticks\":" << battle.frames << '}'; first = false;
        }
        output << "],\"auto_resolves\":["; first = true;
        for (const auto id : auto_resolves_) { output << (first ? "" : ",") << id; first = false; }
        output << "],\"clock_orders\":["; first = true;
        for (const auto& row : clock_orders_) { output << (first ? "" : ",") << row; first = false; }
        output << "],\"clock_changes\":["; first = true;
        for (const auto& row : clock_changes_) { output << (first ? "" : ",") << row; first = false; }
        output << "],\"movements\":["; first = true;
        for (const auto& row : movements_) { output << (first ? "" : ",") << row; first = false; }
        output << "],\"diagnostics\":["; first = true;
        for (const auto& row : diagnostics_) { output << (first ? "" : ",") << std::quoted(row); first = false; }
        output << "]}\n";
    }
    std::uint64_t delay{};
private:
    std::filesystem::path directory_;
    std::string campaign_, last_hash_;
    bool stock_{}, native_ai_{}, paused_{}, resumed_{}, baseline_speed_{}, speed_{}, waiting_{};
    sim::SeatId seat_{};
    std::ofstream hashes_, costs_, trace_;
    std::vector<sim::galactic::Seat> roster_;
    std::shared_ptr<const sim::galactic::GalacticSnapshot> last_snapshot_;
    std::map<sim::SeatCommandKey, net::SeatInput> pending_;
    std::map<sim::EntityId, Battle> battles_;
    std::vector<sim::EntityId> auto_resolves_;
    std::vector<std::string> diagnostics_, clock_orders_, clock_changes_, movements_;
    std::set<sim::EntityId> choices_, summaries_;
    std::uint64_t next_move_{}, last_tick_{}, galaxy_frame_{}, game_frames_{}, ai_steps_{}, stalls_{};
};
} // namespace eawr::presentation::godot_backend
