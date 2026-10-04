#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "live_session_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

namespace {
// GAMECONSTANTS.XML Shield_Flash_Scale and Shield_Flash_Duration (seconds), BP-21.
constexpr std::array<double, 3> shield_flash_scale{1.0, 1.1, 1.25};
constexpr double shield_flash_duration = 0.1;

// #497: how many shooter and target pairs the report keeps the first hit of.
constexpr std::size_t first_hits_limit = 4096;
// #535: how many rows the report's fading_log keeps (one row a fading entity a reached tick).
constexpr std::size_t fading_log_limit = 8192;

} // namespace

void LiveSessionView::apply_time() {
    if (!session_) return;
    session_->set_paused((phase_ != Phase::running && phase_ != Phase::quitting) || !time_.running());
    session_->set_target_rate(time_.target_rate());
}

void LiveSessionView::loading_complete() {
    if (!session_ || phase_ != Phase::loading) return;
    phase_ = Phase::ready;
    ready_tick_ = session_->completed_tick();
    // Driven capture/replay harnesses explicitly admit Begin after finalization.
    // Interactive local battles retain the WBF-10 barrier for the player's input.
    if (!options_.begin_barrier.value_or(options_.real_time)) begin();
}

void LiveSessionView::begin() {
    if (phase_ != Phase::ready) return;
    phase_ = Phase::running;
    begin_tick_ = session_->completed_tick();
    begin_frame_ = shown_frames();
    battle_clock_start_ = std::chrono::steady_clock::now();
    driven_origin_frame_ = last_shown_;
    apply_time();
}

void LiveSessionView::press_pause() {
    if (phase_ == Phase::ready) { begin(); return; }
    if (phase_ != Phase::running) return;
    if (session_ && time_.press_pause(session_->completed_tick())) apply_time();
}

void LiveSessionView::press_fast_forward() {
    if (phase_ != Phase::running) return;
    if (session_ && time_.press_fast_forward(session_->completed_tick())) apply_time();
}

void LiveSessionView::resume() {
    if (phase_ == Phase::ready) { begin(); return; }
    if (phase_ != Phase::running) return;
    if (session_ && time_.resume(session_->completed_tick())) apply_time();
}

void LiveSessionView::quit() {
    if (phase_ == Phase::results || phase_ == Phase::ready) {
        phase_ = Phase::returning;
        quit_ = true;
        return;
    }
    if (phase_ != Phase::running || !scheduler_ || !session_) return;
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::intentional_quit;
    intent.origin = ui::CommandOrigin::hud_button;
    if (!scheduler_->issue(intent)) return;
    phase_ = Phase::quitting;
    // The scheduled command needs one tick even when Quit was pressed while paused.
    static_cast<void>(time_.resume(session_->completed_tick()));
    apply_time();
    if (!options_.real_time) session_->advance_to(scheduler_->open_tick() + 1U);
}

core::Result<SpaceLiveUpdate> LiveSessionView::frame(SpacePopulation& population, GodotRenderer& renderer,
                                                     const double delta) {
    FrameTimer frame_timer(trace_frames_ ? &live_frame_ms_ : nullptr);
    tick_wait_ms_ = session_tail_ms_ = clip_pose_ms_ = 0.0;
    using UpdateResult = core::Result<SpaceLiveUpdate>;
    const auto fail = [](std::string message) {
        return UpdateResult::failure({.code = "EAWR-VIEWER-LIVE-SESSION", .message = std::move(message)});
    };
    // The simulation thread stopped: keep the last poses and carry its error to the view.
    const auto stopped = [this](const core::Diagnostic& failure) {
        simulation_error_ = core::format_diagnostic(failure);
        save_failure_replay();
        SpaceLiveUpdate update;
        update.error = simulation_error_ + "\n"
            + (failure_replay_.empty() ? "No replay saved: " + failure_replay_error_ : "Replay saved: " + failure_replay_);
        return UpdateResult::success(std::move(update));
    };
    if (!session_) return fail("the live session is not running");
    if (const auto failure = session_->failure()) return stopped(*failure);
    ++frames_;
    std::shared_ptr<const tactical::TacticalSnapshot> previous;
    std::shared_ptr<const tactical::TacticalSnapshot> latest;
    double alpha{};
    const std::optional<std::uint64_t> halt = session_->halt_tick();
    if (options_.real_time || phase_ == Phase::ready || phase_ == Phase::quitting
        || phase_ == Phase::results || phase_ == Phase::returning) {
        // Up to one tick behind the session: the presented tick follows it at the time panel's
        // rate (TM-02's whole-millisecond waits) and holds while paused (TM-07, TP-01).
        const platform::LiveFrame frame = session_->frame();
        previous = frame.previous;
        latest = frame.latest;
        const auto newest = static_cast<double>(latest->completed_tick());
        const double rate = (phase_ == Phase::running || phase_ == Phase::quitting) && time_.running()
            ? 1000.0 / static_cast<double>(1000U / time_.target_rate()) : 0.0;
        const double advance = std::isfinite(delta) && delta > 0.0 ? delta * rate : 0.0;
        presented_tick_ = std::clamp(presented_tick_ + advance, std::max(0.0, newest - 1.0), newest);
        alpha = presented_tick_ - (newest - 1.0);
        if (phase_ == Phase::results || phase_ == Phase::returning) {
            // WBF-42/49: a driven pause/quit does not restart its old pacing segment
            // after results open. Keep the final published status and end-frame pose.
            presented_tick_ = newest;
            alpha = 1.0;
        }
        if (phase_ == Phase::ready || phase_ == Phase::quitting) {
            last_shown_ = frames_ > options_.warmup_frames ? frames_ - 1U - options_.warmup_frames : 0U;
        }
    } else {
        const std::uint64_t shown = frames_ > options_.warmup_frames ? frames_ - 1U - options_.warmup_frames : 0U;
        // TP-06: the presented tick advances --eawr-live-step x target / 30 per frame, not at all
        // while paused; a change starts a new segment at the tick the last frame showed.
        const double factor = time_.tick_factor();
        if (factor != driven_factor_) {
            driven_origin_tick_ = driven_base_;
            driven_origin_frame_ = last_shown_;
            driven_factor_ = factor;
        }
        driven_base_ = driven_origin_tick_
            + static_cast<double>(shown - driven_origin_frame_) * options_.ticks_per_frame * driven_factor_;
        last_shown_ = shown;
        presented_tick_ = driven_base_;
        if (options_.stall
            && (options_.stall->start || presented_tick_ > static_cast<double>(options_.stall->tick) + capture_tolerance)) {
            presented_tick_ += static_cast<double>(options_.stall->ticks);
        }
        // BEP-02: the session halts at end_tick; the frames hold there.
        if (halt) presented_tick_ = std::min(presented_tick_, static_cast<double>(*halt));
        auto base = static_cast<std::uint64_t>(std::floor(presented_tick_ + 1.0e-9));
        alpha = std::max(0.0, presented_tick_ - static_cast<double>(base));
        if (halt && base + 1U > *halt && *halt > 0U) {
            base = *halt - 1U;
            alpha = 1.0;
        }
        FrameTimer wait_timer(trace_frames_ ? &tick_wait_ms_ : nullptr);
        session_->advance_to(base + 1U);
        if (!session_->wait_for(base + 1U, std::chrono::seconds(60))) {
            if (const auto failure = session_->failure()) return stopped(*failure);
            return fail("the simulation thread did not reach tick " + std::to_string(base + 1U) + " within 60 s");
        }
        wait_timer.finish();
        previous = session_->snapshot_at(base);
        latest = session_->snapshot_at(base + 1U);
        if (!previous || !latest) return fail("tick " + std::to_string(base) + " left the snapshot history");
    }
    FrameTimer bookkeeping_timer(trace_frames_ ? &bookkeeping_ms_ : nullptr);
    // #518: a squadron a spawner launched registers like a tick-zero one (selection, icon,
    // dogfight grid, leader lookup). A frame may skip ticks, so both ends of it are read.
    for (const auto* snapshot : {previous.get(), latest.get()}) {
        if (snapshot == nullptr) continue;
        for (const tactical::Squadron& squadron : snapshot->squadrons()) register_squadron(squadron, snapshot->completed_tick());
    }
    // The events of the ticks this frame reached, from the session's event log rather than the
    // snapshot history, so a stall longer than the history loses none (#370 review 3). Should
    // the log's own bound have dropped some, the gap is reported, never silently skipped.
    battle_frame_.reached.clear();
    if (latest->completed_tick() > reached_tick_) {
        platform::LiveEvents reached = session_->events_after(reached_tick_, latest->completed_tick());
        if (reached.lost_through) {
            event_gap_rows_.push_back("{\"after\": " + std::to_string(reached_tick_) + ", \"through\": "
                                      + std::to_string(*reached.lost_through) + "}");
            const std::string message = "live session: the events of ticks " + std::to_string(reached_tick_ + 1U)
                + " to " + std::to_string(*reached.lost_through)
                + " left the event log before a frame reached them; their hits and deaths are not shown";
            godot::UtilityFunctions::printerr(godot::String(message.c_str()));
        }
        battle_frame_.reached = std::move(reached.ticks);
    }
    for (const platform::LiveTickEvents& record : battle_frame_.reached) {
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::projectile_hit || first_hits_.size() >= first_hits_limit) continue;
            first_hits_.emplace(std::make_pair(event.shooter, event.target), event.tick);
        }
    }
    // #81: the units destroyed since the last frame's newest tick hand over to their death
    // clones, at the pose the unit was last drawn with (a unit the local player did not see
    // leaves no clone). #447: a craft that spins away keeps its ship along the spin and hands
    // over when the spin ends, at its last spin pose.
    for (const platform::LiveTickEvents& record : battle_frame_.reached) {
        for (const tactical::Event& event : record.events) {
            if (event.kind == tactical::EventKind::pad_structure_sold && event.sequence != sim::invalid_entity_id) {
                pad_empty_since_[event.sequence] = event.tick;
            }
            if (event.kind == tactical::EventKind::station_replaced) {
                // WPR-52: future hangar slots enter the pool only after their station exists.
                if (const auto* replacement = space::find_instance(*latest, event.sequence)) {
                    for (LaunchSlot& slot : launch_slots_) {
                        if (slot.required_station == replacement->type_id && slot.station_owner == replacement->owner) {
                            slot.required_station = 0;
                        }
                    }
                }
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_started) {
                spin_rows_[event.unit] = SpinRow{record.tick, std::nullopt};
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_ended) {
                if (const auto row = spin_rows_.find(event.unit); row != spin_rows_.end()) row->second.ended = record.tick;
            } else if (event.kind != tactical::EventKind::unit_destroyed) {
                continue;
            }
            if (event.kind == tactical::EventKind::unit_destroyed
                && std::any_of(record.events.begin(), record.events.end(), [&event](const tactical::Event& other) {
                       return other.kind == tactical::EventKind::spin_away_started && other.unit == event.unit;
                   })) {
                continue;
            }
            // FW-27: a ghost is knowledge, so a hidden death plays no clone.
            if (!options_.reveal) {
                const auto memory = fog_ghosts_.states().find(event.unit);
                if (memory != fog_ghosts_.states().end() && memory->second.ghost) continue;
            }
            const auto clone = death_clones_.find(event.unit);
            const auto pose = last_poses_.find(event.unit);
            if (clone == death_clones_.end() || pose == last_poses_.end()) continue;
            ActiveClone active{event.unit, record.tick, pose->second};
            active.pose.ship = clone->second.ship;
            active.pose.hardpoints.clear();
            active_clones_.push_back(std::move(active));
        }
    }
    latest_tick_ = latest->completed_tick();
    reached_tick_ = std::max(reached_tick_, latest_tick_);
    ability_snapshot_ = latest;
    for (const tactical::TacticalInstance& instance : latest->instances()) {
        if (instance.ion_stun_frames > 0) {
            auto [row, added] = ion_stun_rows_.try_emplace(instance.entity_id, IonStunRow{latest->completed_tick(), 0U});
            static_cast<void>(added);
            row->second.max_frames = std::max(row->second.max_frames, instance.ion_stun_frames);
        }
        if (!squadron_members_.contains(instance.entity_id)) continue;
        for (const tactical::AbilityStatus& status : instance.abilities) {
            if (status.kind != tactical::AbilityKind::ion_cannon_shot) continue;
            IonShotRow& row = ion_shot_rows_[instance.entity_id];
            if (status.active && !row.on) {
                ++row.switched_on;
                if (row.first_on == 0) row.first_on = latest->completed_tick();
            }
            if (status.active) row.last_on = latest->completed_tick();
            row.on = status.active;
        }
    }
    if (scoring_ && scoring_failure_.empty()) {
        if (auto observed = scoring_->observe(*latest); !observed) {
            scoring_failure_ = core::format_diagnostic(observed.error());
            godot::UtilityFunctions::printerr(godot::String(scoring_failure_.c_str()));
        }
    }
    if (latest->outcome() && outcome_ != latest->outcome()) {
        outcome_ = latest->outcome();
        // #453 BEP-02: the battle ends at end_tick; BE-02: the winner's team wins, every other
        // lobby team loses.
        session_->halt_at(outcome_->end_tick);
        const auto team = team_of_player_.find(player_);
        const bool won = team != team_of_player_.end() && team->second == outcome_->winner_team;
        battle_end_ = BattleEnd{won ? ui::BattleResult::victory : ui::BattleResult::defeat, outcome_->decided_tick,
                                outcome_->end_tick, shown_frames(), std::nullopt};
    }
    if (battle_end_ && !battle_end_->ended_frame
        && presented_tick_ + capture_tolerance >= static_cast<double>(battle_end_->end_tick)) {
        battle_end_->ended_frame = shown_frames();
        results_ = ui::battle_results(*latest, player_, [this](const tactical::TypeId id) {
            for (const auto& type : tables_->units) {
                if (skirmish::type_id(type.id) == id) return ui::ResultType{type.id,
                    type.score_cost_credits.value_or(sim::math::Fixed{}).raw() >= sim::math::Fixed::scale, type.named_hero,
                    type.score_cost_credits.value_or(sim::math::Fixed{}), scoring_ ? scoring_->combat_rating(id)
                        : static_cast<double>(type.score_combat_power.value_or(sim::math::Fixed{}).raw()) / sim::math::Fixed::scale};
            }
            return ui::ResultType{};
        });
        // WBF-44: wall milliseconds start at Begin, excluding loading. Including explicit
        // pause is the current project policy while the retail pause contribution remains U5.
        if (battle_clock_start_) results_.elapsed_milliseconds = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *battle_clock_start_).count());
        if (scoring_ && scoring_failure_.empty()) {
            std::array<tactical::PlayerId, 2> score_players{player_, player_};
            const auto local = std::find_if(latest->players().begin(), latest->players().end(),
                [this](const auto& player) { return player.player_id == player_; });
            for (const auto& player : latest->players()) {
                if (!player.neutral && local != latest->players().end() && player.team_id != local->team_id) {
                    score_players[1] = player.player_id;
                    break;
                }
            }
            for (std::size_t side = 0; side < score_players.size(); ++side) {
                for (const auto control : {side == 0 ? "IDC_YOUR_LOSS_VAL_STATIC" : "IDC_ENEMY_LOSS_VAL_STATIC",
                    "IDC_MILITARY_EFFICIENCY_STATIC", "IDC_KILL_EFFICIENCY_STATIC", "IDC_CONQUEST_EFFICIENCY_STATIC", "IDC_TITLE_STATIC"}) {
                    auto value = scoring_->query(score_players[side], control);
                    if (!value) {
                        scoring_failure_ = core::format_diagnostic(value.error());
                        break;
                    }
                    results_.statistics[side].emplace(control, std::move(value).value());
                }
                const auto score = results_.statistics[side].find(side == 0 ? "IDC_YOUR_LOSS_VAL_STATIC" : "IDC_ENEMY_LOSS_VAL_STATIC");
                if (score != results_.statistics[side].end()) results_.scores[side] = score->second;
                if (!scoring_failure_.empty()) break;
            }
        }
        results_.scoring_diagnostic = scoring_failure_;
        phase_ = Phase::results;
        time_.end(battle_end_->end_tick);
        apply_time();
    }
    battle_frame_.previous = previous;
    battle_frame_.latest = latest;
    battle_frame_.alpha = std::clamp(alpha, 0.0, 1.0);
    battle_frame_.presented_tick = presented_tick_;
    battle_frame_.fog = session_->fog_at(latest->completed_tick());
    // PU-71: one scalar observation, independent of pool size and skipped ticks.
    if (const auto* ledger = local_economy()) {
        ui::PoolNotifications notification;
        notification.observe(*ledger);
        reinforcement_notifications_ = notification.additions;
        reinforcement_notification_tick_ = static_cast<double>(notification.frame);
    }
    // #530 (PU-35, PU-36): evidence of each hyperspace arrival the local player saw start: the tick
    // it was first listed, the first tick it was visible to the local team (frame 35) and the tick
    // it had landed (no arrival frame any more).
    for (const tactical::TacticalInstance& instance : latest->instances()) {
        if (!instance.arrival) continue;
        auto& row = arrivals_[instance.entity_id];
        if (row.first_tick == 0) {
            row.first_tick = latest->completed_tick();
            row.owner = instance.owner;
            row.type = instance.type_id;
        }
        row.last_frame = *instance.arrival;
        if (row.visible_tick == 0 && *instance.arrival >= tactical::arrival_visible_frame) row.visible_tick = latest->completed_tick();
    }
    for (auto& [entity, row] : arrivals_) {
        if (row.landed_tick != 0) continue;
        const auto found = std::find_if(latest->instances().begin(), latest->instances().end(),
            [entity = entity](const tactical::TacticalInstance& instance) { return instance.entity_id == entity; });
        if (found != latest->instances().end() && !found->arrival) row.landed_tick = latest->completed_tick();
    }

    // #535: the local player's per-unit fog fade (space-fog-presentation.md FW-16 to FW-18).
    // `visible_now` is exactly what visible_units() and selection keep reading (unchanged); the
    // fade only adds ghosts of units that just left it, still drawn at their true position while
    // they ease out. `--eawr-live-reveal` (#507's draw bypass) shows every instance at full
    // opacity: the fade is not advanced, so nothing ghosts and no opacity is applied (FW-16).
    snapshot_index_.refresh(latest, player_);
    if (construction_snapshot_ != latest) {
        construction_successors_.clear();
        for (const auto& [child, construction] : presented_construction_) {
            if (const auto* pad = pad_view(construction.parent); pad && pad->state.constructed) {
                construction_successors_.emplace(child, pad->state.constructed);
            }
        }
        presented_construction_.clear();
        for (const auto& pad : latest->pads()) {
            if (pad.construction) presented_construction_.emplace(pad.state.under_construction, *pad.construction);
        }
        construction_snapshot_ = latest;
    }
    if (presentation_types_.empty() && tables_) {
        for (const auto& type : tables_->units) presentation_types_.emplace(skirmish::type_id(type.id), &type);
    }
    const auto& visible_now = snapshot_index_.visible();
    const auto& alive_now = snapshot_index_.alive();
    // FW-24: draw unfogged environment props while retaining the raw sensor
    // list for selection, orders, minimap and simulation consumers.
    if (!unfogged_map_props_.empty()) {
        space::map_prop_draw_visibility(visible_now, alive_now, unfogged_map_props_, draw_visibility_);
    }
    const auto& drawn_now = unfogged_map_props_.empty() ? visible_now : draw_visibility_;
    if (fog_memory_types_.empty() && tables_) {
        for (const auto& type : tables_->units) {
            fog_memory_types_.emplace(skirmish::type_id(type.id),
                std::pair{type.last_state_visible_under_fow, type.initial_state_visible_under_fow});
        }
        for (const auto& type : tables_->obstacles) {
            fog_memory_types_.emplace(skirmish::type_id(type.id),
                std::pair{type.last_state_visible_under_fow, type.initial_state_visible_under_fow});
        }
    }
    auto& immediate = fog_immediate_;
    auto& observations = fog_observations_;
    immediate.clear();
    observations.clear();
    bool fog_lookup_ready = false;
    if (!options_.reveal) {
        for (const auto& instance : latest->instances()) {
            const auto type = fog_memory_types_.find(instance.type_id);
            if (type == fog_memory_types_.end() || !type->second.first) continue;
            immediate.push_back(instance.entity_id);
            observations.push_back({instance.entity_id,
                {to_float(instance.fixed_transform.rows[0][3]), to_float(instance.fixed_transform.rows[1][3]),
                 to_float(instance.fixed_transform.rows[2][3])},
                std::binary_search(drawn_now.begin(), drawn_now.end(), instance.entity_id), type->second.second});
            const auto remembered = fog_ghosts_.states().find(instance.entity_id);
            if (observations.back().visible || remembered == fog_ghosts_.states().end()
                || !remembered->second.known || remembered->second.ghost) continue;
            // FW-26: capture the prior submitted frame only when sight is lost,
            // before idle sampling, live composition or colour changes advance.
            const auto ship = ship_of(instance.entity_id, instance.type_id);
            if (!ship) continue;
            if (!fog_lookup_ready) {
                population.prepare_fog_model_capture();
                fog_lookup_ready = true;
            }
            const auto unit = tables_->find(placed_ships_[*ship].object_id);
            const bool capture = unit && unit->capture_point;
            const auto neutral = neutral_fog_poses_.find(instance.type_id);
            const std::span<const animation::BonePose> neutral_pose = neutral != neutral_fog_poses_.end()
                ? std::span<const animation::BonePose>(neutral->second) : std::span<const animation::BonePose>{};
            if (!population.remember_fog_model(renderer, instance.entity_id, *ship,
                capture ? neutral_pose : std::span<const animation::BonePose>{},
                capture ? neutral_fog_colour_ : std::nullopt)) return fail(population.failure());
        }
        const auto fog = session_->fog_at(latest->completed_tick());
        fog_ghosts_.advance(observations, [&](const std::array<double, 3>& position) {
            if (!fog) return false;
            const double size = to_float(fog->rules.cell_size);
            if (size <= 0) return false;
            const double column = std::floor((position[0] - to_float(fog->rules.map_left)) / size);
            const double row = std::floor((to_float(fog->rules.map_top) - position[1]) / size);
            if (column < 0 || row < 0 || column >= fog->rules.cells_wide || row >= fog->rules.cells_tall) return false;
            return (*fog->values[static_cast<std::size_t>(row)])[static_cast<std::size_t>(column)] != 0;
        });
    }
    population.pose_pending_units(renderer);
    if (!nebula_blend_tick_ || *nebula_blend_tick_ < latest->completed_tick()) {
        const auto first = nebula_blend_tick_ ? *nebula_blend_tick_ + 1 : latest->completed_tick();
        for (auto frame = first; frame <= latest->completed_tick(); ++frame) {
            const auto sampled = frame == latest->completed_tick() ? latest : session_->snapshot_at(frame);
            if (!sampled) { ++nebula_blend_missing_ticks_; continue; }
            for (const auto& instance : sampled->instances()) {
                const auto type = presentation_types_.find(instance.type_id);
                if (type == presentation_types_.end() || !type->second->footprint.hazard.nebula_service) continue;
                nebula_blends_[instance.entity_id].service(instance.in_nebula);
            }
        }
        nebula_blend_tick_ = latest->completed_tick();
        std::erase_if(nebula_blends_, [&](const auto& entry) {
            return !std::binary_search(alive_now.begin(), alive_now.end(), entry.first);
        });
    }
    const double fade_frames = fade_presented_tick_ ? std::max(0.0, presented_tick_ - *fade_presented_tick_) : 0.0;
    fade_presented_tick_ = presented_tick_;
    if (!options_.reveal) fade_.advance(drawn_now, alive_now, fade_frames, immediate);
    std::vector<sim::EntityId> fading_entities;
    for (const sim::EntityId entity : fade_.drawn()) {
        if (!std::binary_search(drawn_now.begin(), drawn_now.end(), entity)) fading_entities.push_back(entity);
    }
    fading_units_ = fading_entities.size();
    // #535: one row a unit that is not at full opacity, for each frame that shows a new newest tick
    // (K-4: a test reads a unit's ramp across several ticks from one report, instead of one run a
    // tick).
    if (latest->completed_tick() != fading_logged_tick_) {
        fading_logged_tick_ = latest->completed_tick();
        const std::uint64_t tick = latest->completed_tick();
        for (const sim::EntityId entity : fade_.drawn()) {
            if (fading_log_rows_.size() >= fading_log_limit) break;
            const auto opacity = fade_.opacity(entity);
            if (!opacity || *opacity >= 1.0F) continue;
            fading_log_rows_.push_back("{\"tick\": " + std::to_string(tick) + ", \"entity\": " + std::to_string(entity)
                + ", \"opacity\": " + std::to_string(*opacity) + "}");
        }
    }

    if (!space::interpolate_visible_units(*previous, *latest, alpha, drawn_now, options_.reveal,
                                         fading_entities, poses_, pose_workers_)) {
        return fail("live unit interpolation failed");
    }
    const auto& poses = poses_;
    // #447: the killed craft spinning away that the local player sees.
    const auto spinning = space::interpolate_spinning(*previous, *latest, alpha, player_, options_.reveal);
    unit_frames_.clear();
    for (const auto* drawn : {&poses, &spinning}) {
        for (const space::LiveUnitPose& pose : *drawn) {
            unit_frames_.emplace(pose.entity, BattleEffects::UnitFrame{pose.position, pose.yaw_degrees, pose.type,
                                                                       pose.roll_degrees, pose.pitch_degrees});
        }
    }
    spinning_drawn_max_ = std::max(spinning_drawn_max_, spinning.size());
    const auto with_ship = std::count_if(spinning.begin(), spinning.end(),
        [this](const space::LiveUnitPose& pose) {
            return ship_of_entity_.contains(pose.entity) || launched_ship_of_entity_.contains(pose.entity);
        });
    spinning_ships_max_ = std::max(spinning_ships_max_, static_cast<std::size_t>(with_ship));
    // #82: what the local player can pick this frame, and what still stands. FW-18's fading
    // ghosts are drawn (below) but never selectable or orderable: they are not in visible_now.
    release_dead_slots(*latest);
    visible_.clear();
    for (const space::LiveUnitPose& pose : poses) {
        if (!pad_visible(pose.entity)) continue;
        if (!options_.reveal && !std::binary_search(visible_now.begin(), visible_now.end(), pose.entity)) continue;
        const auto ship = ship_of(pose.entity, pose.type);
        if (!ship) continue;
        const auto players = latest->players();
        const auto owner = std::lower_bound(players.begin(), players.end(), pose.owner,
            [](const tactical::SnapshotPlayer& player, const tactical::PlayerId id) { return player.player_id < id; });
        const bool neutral = owner == players.end() || owner->player_id != pose.owner || owner->neutral;
        visible_.push_back({pose.entity, *ship, pose.type, pose.owner, pose.owner == player_,
                            tactical::players_hostile(players, player_, pose.owner), pose.position, pose.yaw_degrees, neutral});
    }
    auto& live = live_poses_;
    live.clear();
    live.reserve(poses.size() + spinning.size() + active_clones_.size());
    auto& clip_poses = clip_poses_;
    clip_poses.clear();
    for (const auto* drawn : {&poses, &spinning}) {
        for (const space::LiveUnitPose& pose : *drawn) {
            // Spawned after tick zero without a launch slot (a squadron's container): nothing composed.
            if (!pad_visible(pose.entity)) continue;
            const auto ship = ship_of(pose.entity, pose.type);
            if (!ship) continue;
            SpacePopulation::LivePose placed;
            placed.ship = *ship;
            placed.entity = pose.entity;
            // #506: a squadron craft's pitch too, so it flies nose first (space-fighters FM-02, FM-05).
            std::array<sim::math::Fixed, 6> values{};
            const std::array<double, 6> source{pose.position[0], pose.position[1], pose.position[2], pose.yaw_degrees,
                                               pose.roll_degrees, pose.pitch_degrees};
            for (std::size_t index = 0; index < values.size(); ++index) {
                auto fixed = scene::fixed_from_binary32(static_cast<float>(source[index]));
                if (!fixed) return fail("live unit " + std::to_string(pose.entity) + " pose is not finite");
                values[index] = fixed.value();
            }
            placed.position = {values[0], values[1], values[2]};
            placed.yaw_degrees = values[3];
            placed.roll_degrees = values[4];
            placed.pitch_degrees = values[5];
            if (const auto* pad = pad_view(pose.entity)) {
                if (const auto emptied = pad_empty_since_.find(pose.entity); emptied != pad_empty_since_.end()
                    && !pad->state.constructed && !pad->state.under_construction) {
                    if (const auto* player = population.live_clip(*ship); player && player->playable_frames() > 0) {
                        const double elapsed = std::max(0.0, presented_tick_ - static_cast<double>(emptied->second));
                        const double frame = std::min(elapsed * player->frames_per_second() / tactical::logical_frames_per_second,
                            static_cast<double>(player->playable_frames() - 1));
                        constexpr std::uint32_t subdivisions = 64;
                        clip_poses.push_back({*ship, {static_cast<std::uint64_t>(frame * subdivisions), subdivisions}, 0.0F});
                    }
                }
                const bool neutralizing = pose.owner != economy_.pads.neutral;
                const double progress = static_cast<double>(pad->state.progress.raw()) / sim::math::Fixed::scale;
                const auto owner = progress > 0 && !neutralizing ? pad->state.target : pose.owner;
                const auto colour = player_colour(owner).value_or(std::array<std::uint8_t, 3>{255, 255, 255});
                pad_tints_[pose.entity] = ui::capture_colorization(progress, neutralizing, colour);
            }
            if (const auto* construction = pad_construction(pose.entity)) {
                if (pose.instance && pose.instance->durability && pose.instance->durability->max_hull.raw() > 0) {
                    placed.construction_hull = static_cast<double>(pose.instance->durability->hull.raw())
                        / static_cast<double>(pose.instance->durability->max_hull.raw());
                }
                if (const auto* player = population.live_clip(*ship); player && player->playable_frames() > 0) {
                    const double progress = ui::pad_time_progress(presented_tick_, construction->start_frame,
                                                                 construction->finish_frame);
                    constexpr std::uint32_t subdivisions = 64;
                    const double frame = progress * static_cast<double>(player->playable_frames() - 1);
                    clip_poses.push_back({*ship, {static_cast<std::uint64_t>(frame * subdivisions), subdivisions}, 0.0F});
                }
            }
            // A spinning craft (#447) is dead: no DEFEND shell.
            placed.defend_active = options_.defend && !pose.spinning;
            if (pose.instance != nullptr) {
                for (const tactical::AbilityStatus& ability : pose.instance->abilities) {
                    if (ability.kind == tactical::AbilityKind::defend && ability.active) placed.defend_active = true;
                }
            }
            if (pose.instance != nullptr && pose.instance->durability) {
                for (const tactical::HardpointStatus& hardpoint : pose.instance->durability->hardpoints) {
                    placed.hardpoints.push_back(static_cast<scene::HardpointState>(hardpoint.state));
                }
            }
            // #76 AB-31 (UA-06): the S-foils follow SPOILER_LOCK. A switch starts the other clip at the
            // frame that mirrors the running clip's remaining frames, at the type's deployment rate
            // (1.0 for the X-wing; retail's 1/30 s blend is not drawn), and it holds its last frame.
            // A craft spinning away (#447) keeps the S-foils it died with.
            if (const animation::Player* deploy = population.live_clip(*ship);
                deploy != nullptr && (pose.instance != nullptr || pose.spinning) && population.live_clip(*ship, true) != nullptr) {
                SFoil& foil = sfoils_[pose.entity];
                bool on = foil.on;
                if (pose.instance != nullptr) {
                    on = false;
                    for (const tactical::AbilityStatus& ability : pose.instance->abilities) {
                        if (ability.kind == tactical::AbilityKind::spoiler_lock && ability.active) on = true;
                    }
                }
                const auto frame_of = [&](const SFoil& state) {
                    const animation::Player* player = population.live_clip(*ship, state.alternate);
                    const double frames = player->playable_frames();
                    const double elapsed = std::max(0.0, presented_tick_ - state.since);
                    const double frame = state.start_frame
                        + elapsed * player->frames_per_second() / tactical::logical_frames_per_second;
                    return std::pair{std::min(frame, std::max(0.0, frames - 1.0)), player->playable_frames()};
                };
                if (on != foil.on) {
                    std::int64_t start = 0;
                    if (foil.started) {
                        const auto [frame, count] = frame_of(foil);
                        start = static_cast<std::int64_t>(count) - static_cast<std::int64_t>(frame) - 1;
                    }
                    foil = SFoil{on, true, !on, presented_tick_, static_cast<std::uint32_t>(std::max<std::int64_t>(start, 0))};
                    ++sfoil_switches_;
                }
                if (foil.started) {
                    constexpr std::uint32_t subdivisions = 64;
                    const auto [frame, count] = frame_of(foil);
                    static_cast<void>(count);
                    clip_poses.push_back({*ship, {static_cast<std::uint64_t>(frame * subdivisions), subdivisions}, 0.0F,
                                          foil.alternate});
                }
            }
            last_poses_[pose.entity] = placed;
            live.push_back(std::move(placed));
        }
    }
    std::erase_if(last_poses_, [this](const auto& entry) { return !unit_frames_.contains(entry.first); });
    std::erase_if(pad_tints_, [this](const auto& entry) { return !unit_frames_.contains(entry.first); });
    std::erase_if(pad_empty_since_, [this](const auto& entry) { return snapshot_index_.instance(entry.first) == nullptr; });
    const std::size_t seen = options_.reveal ? poses.size() : visible_now.size();
    visible_units_ = seen;
    hidden_units_ = latest->instances().size() - seen;
    // A clone shows from the first frame past its unit's last drawn tick, its clip clock
    // counting whole ticks of the presentation clock from there.
    // A clone whose clip did not start (no clip of its type, or it did not bind) is removed at
    // once with Remove_Upon_Death and otherwise keeps its pose; one that has faded out leaves.
    // Either way it is no longer drawn, and its ship is retired and its resources released once
    // the frame's unit emitters have run the samples it still stood in (#429).
    retire_clones(population, renderer);
    for (auto active = active_clones_.begin(); active != active_clones_.end();) {
        const DeathClone& clone = death_clones_.at(active->unit);
        const animation::Player* player = population.live_clip(clone.ship);
        const double elapsed = presented_tick_ - (static_cast<double>(active->death_tick) - 1.0);
        const auto tick = static_cast<std::uint64_t>(std::max(0.0, std::floor(elapsed + 1.0e-9)));
        const animation::DeathStart start = animation::death_start(player != nullptr, clone.remove_upon_death);
        const auto death = clone_death_frame(clone, player, tick);
        if (start == animation::DeathStart::removed || (death && !death->shown)) {
            retired_clone_rows_.push_back("{\"unit\": " + std::to_string(active->unit) + ", \"death_tick\": "
                + std::to_string(active->death_tick) + ", \"clone_tick\": " + std::to_string(tick) + ", \"reason\": "
                + json(start == animation::DeathStart::removed ? "removed: clip did not start" : "faded out") + "}");
            retiring_clones_.push_back(std::move(*active));
            active = active_clones_.erase(active);
            continue;
        }
        if (death) {
            live.push_back(active->pose);
            if (player != nullptr) clip_poses.push_back({clone.ship, death->position, death->blend_from});
        }
        ++active;
    }
    // #391: the breakoff props of the hardpoints destroyed so far.
    if (debris_) {
        debris_->pose(battle_frame_.reached, [this](const std::uint64_t tick) { return session_->snapshot_at(tick); },
                      player_, presented_tick_, live, options_.reveal);
    }
    // #456: the model projectiles in flight, each on its pool's placed ship.
    if (projectile_models_) {
        // #862: the ion shots launched since the last frame pose as the ability shot's model.
        projectile_models_->note_ability_shots(battle_frame_.reached, *latest,
            [this](const std::uint64_t tick) { return session_->snapshot_at(tick); });
        projectile_models_->pose_projectile_models(*previous, *latest, battle_frame_.alpha,
            [this](const sim::EntityId entity) { return unit_frame(entity); }, live);
    }
    // WR-12..14: cursor-following clones use the same composed model, scale and idle reader
    // as ordinary ships. Emitters were removed from their private placements at composition.
    if (preview_type_ && preview_point_ && reinforcement_allowed()) {
        const auto* player = economy_.player(player_);
        if (player != nullptr) {
            const auto direction = tactical::planar_direction(player->reinforcement_yaw);
            if (!direction) return fail(core::format_diagnostic(direction.error()));
            std::ostringstream sample;
            sample << "{\"tick\": " << latest->completed_tick() << ", \"valid\": "
                   << (preview_valid_ ? "true" : "false") << ", \"clones\": [";
            std::size_t clone_index = 0;
            for (const auto& clone : placement_clones_) {
                if (clone.type != *preview_type_) continue;
                const auto xx = sim::math::multiply(clone.offset.x, direction.value().x);
                const auto yy = sim::math::multiply(clone.offset.y, direction.value().y);
                const auto xy = sim::math::multiply(clone.offset.x, direction.value().y);
                const auto yx = sim::math::multiply(clone.offset.y, direction.value().x);
                if (!xx || !yy || !xy || !yx) return fail("reinforcement preview offset overflow");
                SpacePopulation::LivePose placed;
                placed.ship = clone.ship;
                placed.position = {sim::math::Fixed::from_raw(preview_point_->x.raw() + xx.value().raw() - yy.value().raw()),
                    sim::math::Fixed::from_raw(preview_point_->y.raw() + xy.value().raw() + yx.value().raw()),
                    sim::math::Fixed::from_raw(clone.layer_z.raw() + clone.offset.z.raw())};
                placed.yaw_degrees = player->reinforcement_yaw;
                live.push_back(placed);
                sample << (clone_index++ ? ", " : "") << "[" << to_float(placed.position.x) << ", "
                       << to_float(placed.position.y) << ", " << to_float(placed.position.z) << "]";
                population.set_live_opacity(renderer, clone.ship, 1.0F); // authored colour alpha is not opacity
                for (const auto entity : population.live_ship_entities(clone.ship)) {
                    renderer.set_light_scale(entity, preview_colours_[preview_valid_ ? 0 : 1]);
                }
            }
            sample << "]}";
            if (preview_rows_.size() < 256) preview_rows_.push_back(sample.str());
            ++preview_frames_;
        }
    }
    bookkeeping_timer.finish();
    population.trace_frames(trace_frames_);
    FrameTimer pose_timer(trace_frames_ ? &pose_ms_ : nullptr);
    if (!population.pose_live(live, pose_workers_)) return fail(population.failure());
    pose_timer.finish();
    FrameTimer opacity_timer(trace_frames_ ? &opacity_ms_ : nullptr);
    std::map<sim::EntityId, std::array<float, 3>> visual_scales;
    // #535: the fog fade's opacity (FW-16 to FW-19) goes to every piece of the ship; the pieces
    // whose adapters carry the unit's opacity uniform (the hull surfaces, as BP-21's shield flash
    // reaches) dither with it, the others ignore it (G-FW14).
    for (const space::LiveUnitPose& pose : poses) {
        const auto ship = ship_of(pose.entity, pose.type);
        if (!ship) continue;
        const auto blend = nebula_blends_.find(pose.entity);
        const float nebula = blend == nebula_blends_.end() || blend->second.value() <= 0.01F ? 0.0F : blend->second.value();
        const float opacity = fade_.opacity(pose.entity).value_or(1.0F)
            * (1.0F + nebula * (nebula_colour_.a / 255.0F - 1.0F));
        population.set_live_opacity(renderer, *ship, opacity);
        // WR-37: frame 35 starts a light fade lasting 115/(FPS*4) seconds.
        // Presentation time interpolates the logical counter; fog opacity stays independent.
        float light = 1.0F;
        if (pose.instance != nullptr && pose.instance->arrival) {
            const double birth = static_cast<double>(latest->completed_tick()) - *pose.instance->arrival;
            light = static_cast<float>(std::clamp((presented_tick_ - birth - tactical::arrival_visible_frame)
                / (static_cast<double>(tactical::arrival_sweep_frames) / 4.0), 0.0, 1.0));
        }
        const auto pad = pad_tints_.find(pose.entity);
        const std::array<float, 3> scale{light * (1.0F + nebula * (nebula_colour_.r / 255.0F - 1.0F)),
            light * (1.0F + nebula * (nebula_colour_.g / 255.0F - 1.0F)),
            light * (1.0F + nebula * (nebula_colour_.b / 255.0F - 1.0F))};
        visual_scales.emplace(pose.entity, scale);
        // WBP-52: capture colour belongs to each material mask, independently of lighting.
        for (const sim::EntityId piece : population.live_ship_entities(*ship)) {
            if (pad != pad_tints_.end()) renderer.set_unit_colorization(piece, pad->second);
            renderer.set_light_scale(piece, scale);
        }
    }
    opacity_timer.finish();
    FrameTimer tail_timer(trace_frames_ ? &session_tail_ms_ : nullptr);
    FrameTimer clip_timer(trace_frames_ ? &clip_pose_ms_ : nullptr);
    if (!population.pose_live_clips(renderer, clip_poses)) return fail(population.failure());
    clip_timer.finish();
    // #427: the shield shells' clock is the presentation clock (seconds).
    population.set_shield_time(renderer, static_cast<float>(presented_tick_ / tactical::logical_frames_per_second));
    // #427/#438 BP-21: opt-in only. A hit the shield took whole starts the colour flash at the tick's
    // start (as its particles are born, BattleEffects), restarting a running one: its own
    // surfaces' light scale RGB starts at Shield_Flash_Scale and returns linearly to 1 over
    // Shield_Flash_Duration. Its hardpoints' attached models are other objects and keep theirs.
    if (options_.shield_flash) {
        for (const platform::LiveTickEvents& record : battle_frame_.reached) {
            for (const tactical::AsteroidImpact& impact : record.asteroid_impacts) {
                if (!impact.outcome.storm_shield_branch) continue;
                shield_flash_start_[impact.target] = static_cast<double>(record.tick) - 1.0;
                ++shield_flashes_;
            }
            for (const tactical::CombatEvent& event : record.combat_events) {
                if (event.kind != tactical::CombatEventKind::projectile_hit
                    || (event.outcome & (tactical::hit_outcome_shield_absorbed | tactical::hit_outcome_storm_shield)) == 0U) {
                    continue;
                }
                shield_flash_start_[event.target] = static_cast<double>(record.tick) - 1.0;
                ++shield_flashes_;
            }
        }
    }
    for (auto flash = shield_flash_start_.begin(); flash != shield_flash_start_.end();) {
        const double seconds = (presented_tick_ - flash->second) / tactical::logical_frames_per_second;
        const bool running = seconds >= 0.0 && seconds < shield_flash_duration;
        std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
        for (std::size_t channel = 0; running && channel < 3; ++channel) {
            const double t = seconds / shield_flash_duration;
            scale[channel] = static_cast<float>(shield_flash_scale[channel] + (1.0 - shield_flash_scale[channel]) * t);
        }
        if (const auto ship = ship_of_entity_.find(flash->first); ship != ship_of_entity_.end()) {
            if (const auto base = visual_scales.find(flash->first); base != visual_scales.end()) {
                for (std::size_t channel = 0; channel < 3; ++channel) scale[channel] *= base->second[channel];
            }
            for (const sim::EntityId entity : population.live_hull_entities(ship->second)) {
                renderer.set_light_scale(entity, scale);
            }
        }
        flash = running ? std::next(flash) : shield_flash_start_.erase(flash);
    }

    SpaceLiveUpdate update;
    fog_ghost_instances_.clear();
    if (!options_.reveal) population.draw_fog_models(renderer, fog_ghosts_, fog_ghost_instances_);
    if (fog_ghost_logged_tick_ != latest->completed_tick()) {
        fog_ghost_logged_tick_ = latest->completed_tick();
        for (const auto& observation : observations) {
            if (fog_ghost_rows_.size() >= 8192) break;
            const auto memory = fog_ghosts_.states().find(observation.entity);
            const bool ghost = memory != fog_ghosts_.states().end() && memory->second.ghost;
            const auto pieces = population.fog_model_pieces(observation.entity);
            const auto state = std::tuple{observation.visible, ghost && pieces != 0, pieces};
            const auto logged = fog_ghost_logged_states_.find(observation.entity);
            if (logged != fog_ghost_logged_states_.end() && logged->second == state) continue;
            fog_ghost_logged_states_.insert_or_assign(observation.entity, state);
            fog_ghost_rows_.push_back("{\"tick\": " + std::to_string(latest->completed_tick())
                + ", \"entity\": " + std::to_string(observation.entity) + ", \"visible\": "
                + (observation.visible ? "true" : "false") + ", \"ghost\": " + (ghost && pieces ? "true" : "false")
                + ", \"pieces\": " + std::to_string(pieces) + ", \"light_scale\": " + (ghost ? "0.5" : "1.0") + "}");
        }
    }
    update.instances.emplace();
    update.instances->reserve(population.instances().size() + fog_ghost_instances_.size());
    update.instances->insert(update.instances->end(), population.instances().begin(), population.instances().end());
    update.instances->insert(update.instances->end(), fog_ghost_instances_.begin(), fog_ghost_instances_.end());
    if (!options_.real_time) {
        // The clock holds at tick 0 through the warm-up frames; nothing is captured before
        // they have drawn. prepare() admits only capture ticks a frame shows, so the frame that
        // reaches one shows exactly that tick, which names the file.
        if (frames_ > options_.warmup_frames && next_capture_ < options_.capture_ticks.size()) {
            const auto wanted = static_cast<double>(options_.capture_ticks[next_capture_]);
            if (presented_tick_ > wanted + capture_tolerance) {
                return fail("capture tick " + std::to_string(options_.capture_ticks[next_capture_])
                            + " was not shown by any frame");
            }
            if (presented_tick_ + capture_tolerance >= wanted) {
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), "_t%04llu",
                              static_cast<unsigned long long>(options_.capture_ticks[next_capture_]));
                update.capture_suffix = suffix;
                ++next_capture_;
            }
        }
        // #459: captures by shown frame, for what a paused battle shows over several frames.
        if (next_capture_frame_ < options_.capture_frames.size()
            && shown_frames() >= options_.capture_frames[next_capture_frame_]) {
            if (update.capture_suffix) {
                return fail("capture frame " + std::to_string(options_.capture_frames[next_capture_frame_])
                            + " shows a capture tick too; ask for one of them");
            }
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_f%04llu",
                          static_cast<unsigned long long>(options_.capture_frames[next_capture_frame_]));
            update.capture_suffix = suffix;
            ++next_capture_frame_;
        }
        const std::uint64_t end = std::max<std::uint64_t>(options_.end_tick.value_or(0U),
            options_.capture_ticks.empty() ? 0U : options_.capture_ticks.back());
        update.hold = presented_tick_ + 1.0e-9 < static_cast<double>(end);
        if (battle_end_ && battle_end_->ended_frame) {
            // BEP-02: the battle halted; the run keeps drawing the frames it would have taken to
            // reach its last tick, so the end panel can be looked at and clicked, then stops.
            const double rest = std::max(0.0, static_cast<double>(end) - static_cast<double>(battle_end_->end_tick));
            const auto frames = static_cast<std::uint64_t>(std::ceil(rest / options_.ticks_per_frame - 1.0e-9));
            update.hold = shown_frames() < *battle_end_->ended_frame + frames;
        }
        if (next_capture_frame_ < options_.capture_frames.size()) update.hold = true;
    }
    update.quit = quit_;
    return UpdateResult::success(std::move(update));
}

void LiveSessionView::finish() {
    if (finished_ || !session_) return;
    finished_ = true;
    shutdown_trace::mark("finish: session stop begins");
    session_->stop();
    shutdown_trace::mark("finish: session stopped");
    hashes_ = session_->tick_hashes();
    const bool verify = options_.verify.value_or(!options_.real_time);
    tactical::TacticalReplay replay;
    if (verify || !options_.replay_path.empty()) replay = session_->record();
    shutdown_trace::mark("finish: " + std::to_string(hashes_.size()) + " ticks, replay verification "
                         + (verify ? "on" : "off"));
    finish_status_ = "stopped";
    if (verify) {
        // The viewer-attached run against a headless run of the same command stream.
        auto headless = platform::headless_tick_hashes(replay, content_->sensors, content_->durability, content_->motion,
                                                       content_->combat, victory_, content_->fog, content_->abilities,
                                                       economy_);
        shutdown_trace::mark("finish: headless replay done");
        headless_equal_ = headless && headless.value() == hashes_;
        if (!headless) finish_status_ = "headless replay failed: " + core::format_diagnostic(headless.error());
    }
    if (const auto failure = session_->failure()) {
        simulation_error_ = core::format_diagnostic(*failure);
        finish_status_ = "simulation_failed";
        save_failure_replay();
    }
    if (!options_.hashes_path.empty()) {
        std::ofstream output(options_.hashes_path, std::ios::binary | std::ios::trunc);
        output << "tick,sha256\n";
        for (std::size_t index = 0; index < hashes_.size(); ++index) output << index + 1 << ',' << hashes_[index] << '\n';
        if (!output) finish_status_ = "could not write " + ViewerPath::utf8(options_.hashes_path);
    }
    if (!options_.replay_path.empty()) {
        auto bytes = tactical::write_replay(replay);
        std::ofstream output(options_.replay_path, std::ios::binary | std::ios::trunc);
        if (bytes) output.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        if (!bytes || !output) finish_status_ = "could not write " + ViewerPath::utf8(options_.replay_path);
        // #459 TP-04: the time track beside the replay; the replay itself is unchanged.
        std::filesystem::path time_path = options_.replay_path;
        time_path += ".time.csv";
        std::ofstream time(time_path, std::ios::binary | std::ios::trunc);
        time << time_.track_csv();
        if (!time) finish_status_ = "could not write " + ViewerPath::utf8(time_path);
    }
}

} // namespace eawr::presentation::godot_backend
