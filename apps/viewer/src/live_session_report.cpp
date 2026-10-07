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

void LiveSessionView::save_failure_replay() {
    if (failure_replay_tried_ || !session_ || !session_->failure()) return;
    failure_replay_tried_ = true;
    const tactical::TacticalReplay replay = session_->failure_record();
    auto bytes = tactical::write_replay(replay);
    if (!bytes) {
        failure_replay_error_ = core::format_diagnostic(bytes.error());
        return;
    }
    // Beside godot.log: Godot's file log setting, user://logs/godot.log by default.
    auto* settings = godot::ProjectSettings::get_singleton();
    const godot::String log = settings->get_setting("debug/file_logging/log_path", godot::String("user://logs/godot.log"));
    const godot::CharString global = settings->globalize_path(log).utf8();
    const std::filesystem::path directory =
        ViewerPath{std::string(global.get_data(), static_cast<std::size_t>(global.length()))}.native().parent_path();
    const auto seconds = static_cast<std::int64_t>(godot::Time::get_singleton()->get_unix_time_from_system());
    // Concurrent lanes may fail at the same second and tick. Include the process
    // and monotonic time so even a shared custom log directory keeps both replays.
    const auto process = godot::OS::get_singleton()->get_process_id();
    const auto usec = godot::Time::get_singleton()->get_ticks_usec();
    const std::filesystem::path path = directory
        / ("eawr-live-failure-" + std::to_string(seconds) + "-pid" + std::to_string(process)
           + "-usec" + std::to_string(usec) + "-tick" + std::to_string(replay.final_tick_count) + ".eawr-replay");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    output.close();
    if (!output) {
        failure_replay_error_ = "could not write " + ViewerPath::utf8(path);
        godot::UtilityFunctions::printerr(godot::String(("live session: " + failure_replay_error_).c_str()));
        return;
    }
    failure_replay_ = ViewerPath::utf8(path);
    godot::UtilityFunctions::print(godot::String(("live session: the failure replay is " + failure_replay_).c_str()));
}

std::vector<platform::LiveTickCost> LiveSessionView::tick_costs_after(const std::uint64_t after) const {
    return session_ ? session_->tick_costs_after(after) : std::vector<platform::LiveTickCost>{};
}

std::shared_ptr<const tactical::TacticalSnapshot> LiveSessionView::snapshot_at(const std::uint64_t tick) const {
    return session_ ? session_->snapshot_at(tick) : nullptr;
}

void LiveSessionView::write_report(std::ostream& output) const {
    output << "  \"live_session\": {\"fixture\": " << json(options_.fixture)
           << ", \"victory_condition\": " << json(tactical::to_string(victory_.condition))
           << ", \"phase\": " << json(phase_ == Phase::loading ? "loading" : phase_ == Phase::ready ? "ready"
                : phase_ == Phase::running ? "running" : phase_ == Phase::quitting ? "quitting"
                : phase_ == Phase::results ? "results" : "returning")
           << ", \"ready_tick\": " << (ready_tick_ ? std::to_string(*ready_tick_) : "null")
           << ", \"begin_tick\": " << (begin_tick_ ? std::to_string(*begin_tick_) : "null")
           << ", \"begin_frame\": " << (begin_frame_ ? std::to_string(*begin_frame_) : "null")
           << ", \"results\": {\"elapsed_milliseconds\": " << results_.elapsed_milliseconds
           << ", \"time\": " << json(ui::battle_time_text(results_.elapsed_milliseconds))
           << ", \"scoring_pumps\": " << (scoring_ ? scoring_->pumps() : 0U)
           << ", \"scoring_diagnostic\": " << json(results_.scoring_diagnostic)
           << ", \"sides\": [" << [&] {
                  std::string sides;
                  for (std::size_t side = 0; side < results_.losses.size(); ++side) {
                      std::string rows;
                      for (const auto& row : results_.losses[side]) {
                          rows += (rows.empty() ? "" : ", ") + std::string("{\"type\": ") + std::to_string(row.type)
                              + ", \"name\": " + json(row.name) + ", \"count\": " + std::to_string(row.count) + "}";
                      }
                      std::string statistics;
                      std::string heroes;
                      for (const auto& row : results_.heroes[side]) {
                          heroes += (heroes.empty() ? "" : ", ") + std::string("{\"type\": ") + std::to_string(row.type)
                              + ", \"name\": " + json(row.name) + ", \"count\": " + std::to_string(row.count) + "}";
                      }
                      for (const auto& [control, value] : results_.statistics[side])
                          statistics += (statistics.empty() ? "" : ", ") + json(control) + ": " + json(value);
                      sides += (sides.empty() ? "" : ", ") + std::string("{\"total\": ") + std::to_string(results_.totals[side])
                          + ", \"score_cost\": " + std::to_string(results_.score_cost[side])
                          + ", \"combat_power\": " + std::to_string(results_.combat_power[side])
                          + ", \"losses\": [" + rows + "], \"heroes\": [" + heroes + "], \"statistics\": {" + statistics + "}}";
                  }
                  return sides;
              }() << "]}"
           << ", \"intentional_quits\": [" << [&] {
                  std::string rows;
                  if (battle_frame_.latest) for (const auto& quit : battle_frame_.latest->quits()) {
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"player\": ") + std::to_string(quit.player)
                          + ", \"tick\": " + std::to_string(quit.tick) + "}";
                  }
                  return rows;
              }() << "]"
           << ", \"replay\": " << json(ViewerPath::utf8(options_.replay_input))
           << ", \"reveal\": " << (options_.reveal ? "true" : "false")
           << ", \"pacing\": " << json(options_.real_time ? "real_time" : "driven")
           << ", \"workers\": " << (session_ ? session_->worker_count() : 0U)
           << ", \"local_player\": " << player_
           << ", \"units\": " << ship_of_entity_.size()
           << ", \"launch_slots\": " << launch_slots_.size()
           << ", \"launched_drawn\": " << launched_ship_of_entity_.size()
           << ", \"slots_released\": " << slots_released_
           << ", \"squadrons\": [" << [&] {
                  // #518: every registered squadron, the setup's and the launched, as first seen.
                  std::string rows;
                  for (const auto& [container, squadron] : squadrons_) {
                      std::string members;
                      for (const sim::EntityId member : squadron.members) {
                          members += (members.empty() ? "" : ", ") + std::to_string(member);
                      }
                      // The owner from the newest snapshot that still holds the container, else 0.
                      tactical::PlayerId owner{};
                      if (battle_frame_.latest) {
                          for (const tactical::TacticalInstance& instance : battle_frame_.latest->instances()) {
                              if (instance.entity_id == container) owner = instance.owner;
                          }
                      }
                      const bool launched = !setup_ || std::none_of(setup_->squadrons.begin(), setup_->squadrons.end(),
                          [&](const tactical::Squadron& start) { return start.container == container; });
                      // #614: how many of its craft have an S-foil clip pair, and how many hold the foils locked.
                      std::size_t foil_craft = 0;
                      std::size_t foils_locked = 0;
                      for (const sim::EntityId member : squadron.members) {
                          if (const auto foil = sfoils_.find(member); foil != sfoils_.end()) {
                              ++foil_craft;
                              foils_locked += foil->second.on ? 1U : 0U;
                          }
                      }
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"container\": ") + std::to_string(container)
                          + ", \"sfoil_craft\": " + std::to_string(foil_craft)
                          + ", \"sfoils_locked\": " + std::to_string(foils_locked)
                          + ", \"owner\": " + std::to_string(owner) + ", \"launched\": " + (launched ? "true" : "false")
                          + ", \"seen_tick\": " + std::to_string(squadron_seen_.at(container))
                          + ", \"members\": [" + members + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"session_records\": " << session_records_.size()
           << ", \"frames\": " << frames_
           << ", \"presented_tick\": " << presented_tick_
           << ", \"latest_tick\": " << latest_tick_
           << ", \"stall\": " << (options_.stall ? "{\"tick\": "
                                                     + (options_.stall->start ? std::string("\"start\"")
                                                                              : std::to_string(options_.stall->tick))
                                                     + ", \"ticks\": " + std::to_string(options_.stall->ticks) + "}"
                                                 : std::string("null"))
           << ", \"visible_units\": " << visible_units_
           << ", \"hidden_units\": " << hidden_units_
           << ", \"fading_units\": " << fading_units_
           << ", \"fog_ghost_pieces\": " << fog_ghost_instances_.size()
           << ", \"fog_ghost_log\": [" << [&] {
                  std::string rows;
                  for (const auto& row : fog_ghost_rows_) rows += (rows.empty() ? "" : ", ") + row;
                  return rows;
              }() << "]"
           << ", \"fading\": ["
           << [&] {
                  std::string rows;
                  for (const sim::EntityId entity : fade_.drawn()) {
                      const auto opacity = fade_.opacity(entity);
                      if (!opacity) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"entity\": ") + std::to_string(entity)
                          + ", \"opacity\": " + std::to_string(*opacity) + "}";
                  }
                  return rows;
              }()
           << "]"
           << ", \"fading_log\": [" << [&] {
                  std::string rows;
                  for (const std::string& row : fading_log_rows_) rows += (rows.empty() ? "" : ", ") + row;
                  return rows;
              }() << "]"
           << ", \"scripted_inputs\": " << options_.inputs.size()
           << ", \"start_map\": " << json(start_ ? start_->map : "")
           << ", \"start_map_sha256\": " << json(start_ ? start_->map_sha256 : "")
           << ", \"match_policy\": {\"heroes\": " << (start_ && start_->match.allow_heroes ? "true" : "false")
           << ", \"superweapons\": " << (start_ && start_->match.allow_superweapons ? "true" : "false")
           << ", \"free_starting_units\": " << (start_ && start_->match.free_starting_units ? "true" : "false") << "}"
           << ", \"free_starting_forces\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& unit : start_->units) {
                      if (unit.role != skirmish::UnitRole::free_unit) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"player\": ") + std::to_string(unit.state.owner)
                          + ", \"type\": " + json(unit.type) + "}";
                  }
                  return rows;
              }() << "]"
           << ", \"start_slots\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& player : start_->players) {
                      if (!player.lobby) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"slot\": ")
                          + std::to_string(player.player.player_id) + ", \"faction\": " + json(player.faction)
                          + ", \"team\": " + std::to_string(player.player.team_id)
                          + ", \"human\": " + (player.human ? "true" : "false") + "}";
                  }
                  return rows;
              }() << "]"
           << ", \"start_seed\": " << (setup_ ? setup_->seed : 0)
           << ", \"start_colours\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& player : start_->players) {
                      if (!player.lobby || !player.colour) continue;
                      const auto& rgb = player.colour->rgb;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"player\": ")
                          + std::to_string(player.player.player_id) + ", \"constant\": " + json(player.colour->constant)
                          + ", \"rgb\": [" + std::to_string(rgb[0]) + ", " + std::to_string(rgb[1]) + ", " + std::to_string(rgb[2]) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"rendered_colours\": [" << [&] {
                  std::string rows;
                  for (const auto& ship : placed_ships_) {
                      if (!ship.live_entity || !ship.team_colour) continue;
                      const auto found = owner_of_entity_.find(ship.live_entity);
                      const auto owner = found == owner_of_entity_.end() ? 0U : found->second;
                      const auto& rgb = *ship.team_colour;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"entity\": ")
                          + std::to_string(ship.live_entity) + ", \"player\": " + std::to_string(owner)
                          + ", \"rgb\": [" + std::to_string(rgb[0]) + ", " + std::to_string(rgb[1]) + ", " + std::to_string(rgb[2]) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"start_markers\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& marker : start_->markers) {
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"record\": ") + std::to_string(marker.record)
                          + ", \"player\": " + std::to_string(marker.player) + ", \"use\": " + json(skirmish::to_string(marker.use))
                          + ", \"position\": [" + std::to_string(to_float(marker.position.x)) + ", "
                          + std::to_string(to_float(marker.position.y)) + ", " + std::to_string(to_float(marker.position.z)) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"start_fleet\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& unit : start_->units) {
                      if (unit.role != skirmish::UnitRole::fleet && unit.role != skirmish::UnitRole::free_unit) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"entity\": ") + std::to_string(unit.state.entity_id)
                          + ", \"player\": " + std::to_string(unit.state.owner) + ", \"record\": " + std::to_string(unit.record)
                          + ", \"type\": " + json(unit.type) + ", \"position\": [" + std::to_string(to_float(unit.state.position.x))
                          + ", " + std::to_string(to_float(unit.state.position.y)) + ", "
                          + std::to_string(to_float(unit.state.position.z)) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"ai_flag_ignored\": " << (ai_flag_ignored_ ? "true" : "false")
           << ", \"time\": {\"speed_step\": " << time_.speed_step() << ", \"state\": "
           << json(time_.ended() ? std::string("ended") : std::string(ui::to_string(time_.state())))
           << ", \"target_rate\": " << time_.target_rate() << ", \"track\": ["
           << [&] {
                  std::string rows;
                  for (const ui::TimeChange& change : time_.track()) {
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"tick\": ") + std::to_string(change.tick)
                          + ", \"state\": " + json(ui::to_string(change.state)) + ", \"ticks_per_second\": "
                          + std::to_string(change.target_rate) + ", \"cause\": " + json(change.cause) + "}";
                  }
                  return rows;
              }()
           << "]}"
           << ", \"defend\": " << (options_.defend ? "true" : "false")
           << ", \"shield_flash\": " << (options_.shield_flash ? "true" : "false")
           << ", \"shield_flashes\": " << shield_flashes_
           << ", \"shield_flash_samples\": " << shield_flash_samples_
           << ", \"shield_flash_duration\": " << shield_flash_constants_.duration
           << ", \"max_shield_flash_scale\": [" << max_shield_flash_scale_[0] << ','
           << max_shield_flash_scale_[1] << ',' << max_shield_flash_scale_[2] << ']'

           << ", \"nebula_blends\": [" << [&] {
                  std::ostringstream rows;
                  bool first = true;
                  for (const auto& [id, blend] : nebula_blends_) {
                      rows << (first ? "" : ", ") << "{\"entity\": " << id << ", \"blend\": " << blend.value() << "}";
                      first = false;
                  }
                  return rows.str();
              }() << "]"
           << ", \"nebula_blend_missing_ticks\": " << nebula_blend_missing_ticks_
           << ", \"own_units\": [";
    bool first_unit = true;
    for (const VisibleUnit& unit : visible_) {
        if (!unit.own) continue;
        output << (first_unit ? "" : ", ") << "{\"entity\": " << unit.entity << ", \"position\": [" << unit.position[0]
               << ", " << unit.position[1] << ", " << unit.position[2] << "], \"yaw\": " << unit.yaw << "}";
        first_unit = false;
    }
    output << "]"
           << ", \"drawn_launch_colours\": [";
    bool first_launch = true;
    for (const VisibleUnit& unit : visible_) {
        if (!launched_ship_of_entity_.contains(unit.entity)) continue;
        const auto& ship = placed_ships_[unit.ship];
        if (!ship.team_colour) continue;
        const auto& rgb = *ship.team_colour;
        output << (first_launch ? "" : ", ") << "{\"entity\": " << unit.entity
               << ", \"player\": " << unit.owner << ", \"type\": " << json(ship.object_id)
               << ", \"rgb\": [" << std::to_string(rgb[0]) << ", " << std::to_string(rgb[1]) << ", " << std::to_string(rgb[2]) << "]";
        const auto clone = death_clones_.find(unit.entity);
        if (clone != death_clones_.end() && placed_ships_[clone->second.ship].team_colour) {
            const auto& death_rgb = *placed_ships_[clone->second.ship].team_colour;
            output << ", \"death_rgb\": [" << std::to_string(death_rgb[0]) << ", " << std::to_string(death_rgb[1]) << ", " << std::to_string(death_rgb[2]) << "]";
        }
        output << "}";
        first_launch = false;
    }
    output << "]"
           << ", \"hostile_units\": [";
    bool first_hostile = true;
    for (const VisibleUnit& unit : visible_) {
        if (!unit.hostile) continue;
        output << (first_hostile ? "" : ", ") << "{\"entity\": " << unit.entity << "}";
        first_hostile = false;
    }
    output << "]"
           << ", \"orders\": [";
    for (std::size_t index = 0; index < options_.orders.size(); ++index) {
        const ScheduledOrder& order = options_.orders[index];
        output << (index ? ", " : "") << "{\"tick\": " << order.tick << ", \"kind\": " << json(tactical::to_string(order.kind))
               << ", \"unit\": " << order.unit;
        if (order.target != 0) output << ", \"target\": " << order.target;
        output << "}";
    }
    output << "], \"first_hits\": [";
    for (auto hit = first_hits_.begin(); hit != first_hits_.end(); ++hit) {
        output << (hit == first_hits_.begin() ? "" : ", ") << "{\"shooter\": " << hit->first.first
               << ", \"target\": " << hit->first.second << ", \"tick\": " << hit->second << "}";
    }
    output << "], \"rejected\": [";
    if (session_) {
        const auto rejected = session_->rejected_orders();
        for (std::size_t index = 0; index < rejected.size(); ++index) {
            output << (index ? ", " : "") << json(rejected[index].code + " " + rejected[index].message);
        }
    }
    output << "], \"ai\": ";
    if (session_ && ai_scripts_) {
        // #79: the FoC AI's players, Lua load and the calls it could not make.
        const auto report = session_->script_report();
        output << "{\"players\": [";
        for (std::size_t index = 0; index < ai_players_.size(); ++index) output << (index ? ", " : "") << ai_players_[index];
        output << "], \"ticks\": " << report.ticks << ", \"max_instructions\": " << report.max_instructions
               << ", \"total_instructions\": " << report.total_instructions << ", \"diagnostics\": [";
        for (std::size_t index = 0; index < report.diagnostics.size(); ++index) {
            output << (index ? ", " : "") << json(report.diagnostics[index]);
        }
        output << "]}";
    } else {
        output << "null";
    }
    output << ", \"unit_clips\": [";
    for (std::size_t index = 0; index < unit_clip_rows_.size(); ++index) output << (index ? ", " : "") << unit_clip_rows_[index];
    output << "], \"ion_shots\": {\"squadrons\": [";
    for (auto row = ion_shot_rows_.begin(); row != ion_shot_rows_.end(); ++row) {
        output << (row == ion_shot_rows_.begin() ? "" : ", ") << "{\"squadron\": " << row->first << ", \"switched_on\": "
               << row->second.switched_on << ", \"first_on\": " << row->second.first_on << ", \"last_on\": "
               << row->second.last_on << ", \"on\": " << (row->second.on ? "true" : "false") << "}";
    }
    output << "], \"stunned\": [";
    for (auto row = ion_stun_rows_.begin(); row != ion_stun_rows_.end(); ++row) {
        output << (row == ion_stun_rows_.begin() ? "" : ", ") << "{\"unit\": " << row->first << ", \"first\": "
               << row->second.first << ", \"max_frames\": " << row->second.max_frames << "}";
    }
    output << "]}, \"sfoil_switches\": " << sfoil_switches_ << ", \"ability_requests\": {\"issued\": "
           << abilities_.issued() << ", \"refused\": " << abilities_.refused() << "}";
    // #530: the economy requests, the local player's economy at the last frame and the arrivals.
    output << ", \"pads\": [";
    if (const auto snapshot = battle_frame_.latest) {
        for (std::size_t index = 0; index < snapshot->pads().size(); ++index) {
            const auto& pad = snapshot->pads()[index];
            const auto instance = std::find_if(snapshot->instances().begin(), snapshot->instances().end(),
                [&](const auto& unit) { return unit.entity_id == pad.entity; });
            output << (index ? ", " : "") << "{\"entity\": " << pad.entity << ", \"owner\": "
                << (instance != snapshot->instances().end() ? instance->owner : 0) << ", \"target\": "
                << pad.state.target << ", \"progress_raw\": " << pad.state.progress.raw()
                << ", \"under_construction\": " << pad.state.under_construction
                << ", \"constructed\": " << pad.state.constructed;
            if (pad.construction) {
                output << ", \"start_frame\": " << pad.construction->start_frame
                    << ", \"finish_frame\": " << pad.construction->finish_frame
                    << ", \"build_progress\": " << ui::pad_time_progress(presented_tick_,
                        pad.construction->start_frame, pad.construction->finish_frame);
            }
            output << "}";
        }
    }
    output << "], \"economy_requests\": {\"buys\": " << economy_requests_.buys << ", \"cancels\": "
           << economy_requests_.cancels << ", \"reinforcements\": " << economy_requests_.reinforcements
           << ", \"refused\": " << economy_requests_.refused << "}, \"economy\": ";
    if (const tactical::EconomyView* view = local_economy()) {
        output << "{\"credits\": " << json(ui::credits_text(view->credits)) << ", \"population\": " << view->population
               << ", \"population_cap\": " << view->population_cap << ", \"queued\": ["
               << view->queues[0].size() << ", " << view->queues[1].size() << "], \"pool\": [";
        for (std::size_t index = 0; index < view->pool.size(); ++index) output << (index ? ", " : "") << view->pool[index];
        output << "], \"tech_level\": " << view->tech_level << ", \"completed\": [";
        for (std::size_t index = 0; index < view->completed.size(); ++index) {
            const auto& held = view->completed[index];
            output << (index ? ", " : "") << "{\"type\": " << held.type << ", \"station\": "
                   << held.station << ", \"object\": " << held.object << "}";
        }
        output << "]}";
    } else {
        output << "null";
    }
    output << ", \"placement_preview\": {\"frames\": " << preview_frames_ << ", \"queries\": "
           << preview_queries_ << ", \"samples\": [";
    for (std::size_t index = 0; index < preview_rows_.size(); ++index) output << (index ? ", " : "") << preview_rows_[index];
    output << "]}, \"arrivals\": [";
    std::size_t arrival_index = 0;
    for (const auto& [entity, row] : arrivals_) {
        output << (arrival_index++ ? ", " : "") << "{\"unit\": " << entity << ", \"owner\": " << row.owner
               << ", \"type\": " << row.type << ", \"first_tick\": " << row.first_tick << ", \"visible_tick\": "
               << row.visible_tick << ", \"landed_tick\": " << row.landed_tick << ", \"facing_yaw\": " << row.facing_yaw << "}";
    }
    output << "], \"death_clones\": [";
    for (std::size_t index = 0; index < death_clone_rows_.size(); ++index) output << (index ? ", " : "") << death_clone_rows_[index];
    output << "], \"death_clones_shown\": [";
    for (std::size_t index = 0; index < active_clones_.size(); ++index) {
        output << (index ? ", " : "") << "{\"unit\": " << active_clones_[index].unit
               << ", \"death_tick\": " << active_clones_[index].death_tick << "}";
    }
    output << "], \"death_clones_retired\": [";
    for (std::size_t index = 0; index < retired_clone_rows_.size(); ++index) {
        output << (index ? ", " : "") << retired_clone_rows_[index];
    }
    output << "], \"spin_away\": {\"drawn_max\": " << spinning_drawn_max_ << ", \"ships_max\": " << spinning_ships_max_
           << ", \"spins\": [";
    bool first_spin = true;
    for (const auto& [unit, row] : spin_rows_) {
        output << (first_spin ? "" : ", ") << "{\"unit\": " << unit << ", \"started\": " << row.started
               << ", \"ended\": " << (row.ended ? std::to_string(*row.ended) : std::string("null")) << "}";
        first_spin = false;
    }
    output << "]}, \"event_gaps\": [";
    for (std::size_t index = 0; index < event_gap_rows_.size(); ++index) output << (index ? ", " : "") << event_gap_rows_[index];
    output << "], \"completed_ticks\": " << hashes_.size()
           << ", \"final_state_sha256\": " << json(hashes_.empty() ? "" : hashes_.back())
           << ", \"headless_hashes_equal\": " << (headless_equal_ ? (*headless_equal_ ? "true" : "false") : "null")
           << ", \"outcome\": ";
    if (outcome_) {
        // The local player's result: the winner's team won, every other lobby team lost.
        const auto team = team_of_player_.find(player_);
        const bool won = team != team_of_player_.end() && team->second == outcome_->winner_team;
        output << "{\"condition\": " << json(tactical::to_string(outcome_->condition)) << ", \"winner\": "
               << outcome_->winner << ", \"winner_team\": " << outcome_->winner_team << ", \"decided_tick\": "
               << outcome_->decided_tick << ", \"deciding_unit\": " << outcome_->deciding_unit
               << ", \"end_tick\": " << outcome_->end_tick << ", \"local_result\": " << json(won ? "victory" : "defeat")
               << "}";
    } else {
        output << "null";
    }
    output << ", \"battle_end\": ";
    if (battle_end_) {
        output << "{\"message\": " << json(ui::battle_message_key(battle_end_->result))
               << ", \"title\": " << json(ui::battle_end_title_key(battle_end_->result))
               << ", \"decided_tick\": " << battle_end_->decided_tick << ", \"end_tick\": " << battle_end_->end_tick
               << ", \"shown_frame\": " << battle_end_->shown_frame << ", \"ended_frame\": "
               << (battle_end_->ended_frame ? std::to_string(*battle_end_->ended_frame) : std::string("null"))
               << ", \"halt_tick\": "
               << (session_ && session_->halt_tick() ? std::to_string(*session_->halt_tick()) : std::string("null"))
               << ", \"quit\": " << (quit_ ? "true" : "false") << "}";
    } else {
        output << "null";
    }
    output << ", \"status\": " << json(finish_status_)
           << ", \"error\": " << (simulation_error_.empty() ? std::string("null") : json(simulation_error_))
           << ", \"failure_replay\": " << (failure_replay_.empty() ? std::string("null") : json(failure_replay_)) << "},\n";
}

} // namespace eawr::presentation::godot_backend
