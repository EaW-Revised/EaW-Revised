#include "viewer_load_notes.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/battle/start.hpp"
#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"

#include "battle_source_internal.hpp"
#include "battle_source_draw.hpp"
#include "eawr/presentation/live/live_units.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/presentation/ui/minimap.hpp"
#include "eawr/units/unit_tables.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "live_session_internal.hpp"
#include "lan_match_internal.hpp"
#include <fstream>
#include <iterator>

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

bool LiveSessionView::prepare_source(const BattleSource& source, const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const std::string_view map_path, std::optional<battle::ArrivalPlan>& arrivals,
    std::string& failure) {
    BattlePreparation prepared(options_);
    auto& state = prepared.state();
    state.start_ = std::move(start_);
    state.ai_scripts_ = std::move(ai_scripts_);
    state.ai_players_ = std::move(ai_players_);
    state.setup_ = std::move(setup_);
    state.replay_ = std::move(replay_);
    state.tables_ = std::move(tables_);
    state.content_ = std::move(content_);
    state.victory_ = std::move(victory_);
    state.economy_ = std::move(economy_);
    state.draw_bindings_->placed_ships = std::move(placed_ships_);
    state.session_records_ = std::move(session_records_);
    state.unfogged_map_props_ = std::move(unfogged_map_props_);
    state.ship_of_entity_ = std::move(ship_of_entity_);
    state.owner_of_entity_ = std::move(owner_of_entity_);
    state.team_of_player_ = std::move(team_of_player_);
    state.preview_colours_ = std::move(preview_colours_);
    state.scoring_ = std::move(scoring_);
    state.player_ = std::move(player_);
    state.colorization_enabled_ = std::move(colorization_enabled_);
    const bool ready = source.prepare(prepared, filesystem, catalog, map_path, failure);
    start_ = std::move(state.start_);
    arrivals = std::move(state.arrivals_);
    ai_scripts_ = std::move(state.ai_scripts_);
    ai_players_ = std::move(state.ai_players_);
    setup_ = std::move(state.setup_);
    replay_ = std::move(state.replay_);
    tables_ = std::move(state.tables_);
    content_ = std::move(state.content_);
    victory_ = std::move(state.victory_);
    economy_ = std::move(state.economy_);
    placed_ships_ = std::move(state.draw_bindings_->placed_ships);
    session_records_ = std::move(state.session_records_);
    unfogged_map_props_ = std::move(state.unfogged_map_props_);
    ship_of_entity_ = std::move(state.ship_of_entity_);
    owner_of_entity_ = std::move(state.owner_of_entity_);
    team_of_player_ = std::move(state.team_of_player_);
    preview_colours_ = std::move(state.preview_colours_);
    scoring_ = std::move(state.scoring_);
    player_ = std::move(state.player_);
    colorization_enabled_ = std::move(state.colorization_enabled_);
    return ready;
}

bool LiveSessionView::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view map_path,
                              std::string& failure) {
    if (options_.land) return prepare_land(filesystem, catalog, map_path, failure);
    shield_flash_constants_ = presentation_constants::load_flash(filesystem);
    auto preview_colours = ui::reinforcement_colours(filesystem);
    // Missing mod presentation constants keep the model's ordinary light multiplier.
    preview_colours_ = preview_colours ? preview_colours.value()
        : ui::ReinforcementColours{{{1.0F, 1.0F, 1.0F}, {1.0F, 1.0F, 1.0F}}};
    if (!preview_colours) godot::UtilityFunctions::printerr(godot::String(
        ("reinforcement preview colours: " + core::format_diagnostic(preview_colours.error())
         + "; using untinted previews").c_str()));
    deployment_facing_.reset(); // WR-X01: per local player, per battle
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    const auto minimap_settings = ui::minimap_settings(filesystem);
    nebula_colour_ = minimap_settings.nebula;
    if (const auto neutral = ui::faction_colour(minimap_settings, "Neutral")) {
        neutral_fog_colour_ = std::array<float, 3>{neutral->r / 255.0F, neutral->g / 255.0F, neutral->b / 255.0F};
    }
    faction_colours_ = minimap_settings;
    // #3076: FoC enables unit colorization for the skirmish game mode only; a campaign battle runs without it.
    colorization_enabled_ = !options_.borrowed_battle;
    const skirmish::Fixture& fixture = skirmish::m2_fixture();
    if (!options_.borrowed_battle && (options_.fixture == "replay") != !options_.replay_input.empty()) {
        failure = "--eawr-live-replay goes with --eawr-live-session replay, which needs it";
        return false;
    }
    if ((options_.fixture == "m2" || options_.fixture == "melee") && lower_path(map_path) != lower_path(fixture.map)) {
        failure = "--eawr-live-session " + options_.fixture + " runs on " + fixture.map;
        return false;
    }
    const bool demo = options_.fixture == "attract";
    if (options_.fixture != "skirmish" && !demo
        && (options_.skirmish.map || options_.skirmish.slots || options_.skirmish.seed)) {
        failure = "--eawr-skirmish-* options require --eawr-live-session skirmish";
        return false;
    }
    if (options_.skirmish.victory_condition && options_.fixture != "skirmish" && options_.fixture != "replay") {
        failure = "--eawr-skirmish-victory requires a skirmish start or replay";
        return false;
    }
    if (demo && options_.skirmish.slots) {
        failure = "a demo map's start has no lobby slots";
        return false;
    }
    if (options_.fixture == "skirmish" || demo) {
        if (options_.skirmish.map && *options_.skirmish.map != lower_path(map_path)) {
            failure = "--eawr-map and --eawr-skirmish-map must select the same map";
            return false;
        }
        options_.skirmish.map = lower_path(map_path);
    }
    if (options_.fixture != "melee" && (options_.melee_size || options_.melee_seed)) {
        failure = "--eawr-live-melee and --eawr-live-melee-seed go with --eawr-live-session melee";
        return false;
    }
    const BattleSource* source = battle_source(options_);
    const bool configurable_ai = source && source->ai_capability() == BattleAiCapability::configurable;
    if (!configurable_ai && options_.ai.has_value()) {
        ai_flag_ignored_ = true;
        godot::UtilityFunctions::printerr(godot::String(
            ("--eawr-live-ai has no effect with --eawr-live-session " + options_.fixture + "; it only runs the AI"
             + " of m2 or skirmish starts").c_str()));
    }
    std::optional<battle::ArrivalPlan> selected_arrivals;
    if (!prepare_source(source ? *source : *local_battle_source("m2"), filesystem, catalog, map_path,
                        selected_arrivals, failure)) {
        return false;
    }
    if (!colorization_enabled_) {
        // #3076: a type's own nonzero No_Colorization_Color wins over its owner faction's.
        const auto read_type = [&](const std::string& id) {
            if (type_no_colorization_.contains(id)) return;
            const auto colour = ui::minimap_type_looks(id, &catalog).no_colorization;
            if (colour && *colour != data::ui::Rgba8{0, 0, 0, 0}) type_no_colorization_.emplace(id, std::array<std::uint8_t, 3>{colour->r, colour->g, colour->b});
        };
        for (const auto& type : tables_->units) read_type(type.id);
        for (auto& ship : placed_ships_) {
            read_type(ship.object_id);
            const auto owner = owner_of_entity_.find(ship.live_entity);
            if (owner != owner_of_entity_.end()) ship.team_colour = model_colour(owner->second, ship.object_id);
        }
    }
    // #427: a type with a DEFEND ability composes its SHIELD sub-object as its shield shell.
    std::map<tactical::PlayerId, std::string> scoring_factions;
    for (const auto& player : setup_->players) scoring_factions.emplace(player.player_id, player_faction(player.player_id));
    if (!scoring_) {
        auto scoring = BattleScoring::create(filesystem, catalog, *tables_, std::move(scoring_factions), std::string(map_path));
        if (scoring) scoring_ = std::move(scoring).value();
        else scoring_failure_ = core::format_diagnostic(scoring.error());
    }
    if (!scoring_failure_.empty()) godot::UtilityFunctions::printerr(godot::String(scoring_failure_.c_str()));
    // #76 AB-31 (UA-06): a type with SPOILER_LOCK plays its DEPLOY clip when the ability switches
    // on and UNDEPLOY when it switches off. Retail draws among several variants; the X-wing has one.
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    // FW-30: capture-point copies use neutral IDLE art, independent of their live owner.
    for (const auto& type : tables_->units) {
        if (!type.capture_point || !type.last_state_visible_under_fow) continue;
        if (!neutral_fog_colour_) { failure = "fog capture-point copy: neutral faction colour is unavailable"; return false; }
        auto object = catalog.resolve(type.id, data::Category::game_object);
        const auto clips = animation::model_clip_paths(type.model_path, animation::clip_types::idle, exists,
            object ? tag_text(object.value(), "Space_Model_Anim_Override_Name") : std::string{});
        const auto index = animation::death_clip_variant(clips.size(), type.neutral_fog_animation_index, 0);
        if (!index) continue; // No matching native clip: preserve the copied pose.
        auto model = scene::VfsAssetCache(filesystem).shared_model(type.model_path);
        auto clip = assets::load_animation(filesystem, clips[*index]);
        if (!model || !clip) { failure = "fog capture-point copy: neutral animation asset is unreadable"; return false; }
        auto player = animation::Player::create(*model.value(), &clip.value());
        if (!player) { failure = core::format_diagnostic(player.error()); return false; }
        auto pose = player.value().sample({});
        if (!pose) { failure = core::format_diagnostic(pose.error()); return false; }
        for (auto& bone : pose.value().bones) {
            if (bone.visible) continue;
            for (auto* matrix : {&bone.skin_asset, &bone.model_asset}) {
                for (std::size_t component = 0; component < 12; ++component) (*matrix)[component] = 0.0F;
            }
        }
        neutral_fog_poses_.emplace(battle::type_id(type.id), std::move(pose.value().bones));
    }
    const auto prepare_sfoil_clips = [&] {
        for (BattleScene::PlacedUnit& ship : placed_ships_) {
            const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
                                           [&](const units::UnitType& entry) { return entry.id == ship.object_id; });
            if (!ship.clip.empty() || type == tables_->units.end()
                || std::none_of(type->abilities.begin(), type->abilities.end(),
                                [](const units::Ability& ability) { return ability.type == "SPOILER_LOCK"; })) {
                continue;
            }
            auto object = catalog.resolve(ship.object_id, data::Category::game_object);
            if (!object) continue;
            const std::string model = space_model_path(object.value());
            const std::string anim_override = tag_text(object.value(), "Space_Model_Anim_Override_Name");
            const auto deploy = animation::model_clip_paths(model, animation::clip_types::deploy, exists, anim_override);
            const auto undeploy = animation::model_clip_paths(model, animation::clip_types::undeploy, exists, anim_override);
            if (deploy.empty() || undeploy.empty()) continue;
            ship.clip = deploy.front();
            ship.alternate_clip = undeploy.front();
        }
    };
    prepare_sfoil_clips();
    prepare_clips(filesystem, catalog);
    prepare_launch_slots(filesystem, catalog, std::move(selected_arrivals));
    // HCB-12: one reusable authored model per issuer, never a selectable unit.
    if (setup_) for (const auto& rule : setup_->beacon_rules) {
        const auto type = std::find_if(catalog.definitions().begin(), catalog.definitions().end(),
            [&](const auto& value) { return value.category == data::Category::game_object && battle::type_id(value.id) == rule.type; });
        if (type == catalog.definitions().end()) { failure = "beacon type is absent from the presentation catalog"; return false; }
        BattleScene::PlacedUnit ship;
        ship.object_id = type->id;
        ship.live_entity = std::numeric_limits<sim::EntityId>::max() / 16U + rule.player;
        ship.beacon_model = true;
        ship.team_colour = model_colour(rule.player, ship.object_id);
        beacon_ships_.emplace(rule.player, placed_ships_.size());
        placed_ships_.push_back(std::move(ship));
    }
    // BP-22: purchased reinforcements need the same shell as starting ships, including
    // heroes with DEFEND in their second slot. Include their newly prepared arrival slots.
    for (BattleScene::PlacedUnit& ship : placed_ships_) {
        const auto* type = tables_->find(ship.object_id);
        ship.defend_shell = type != nullptr && !ship.placement_preview
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "DEFEND"; });
        ship.stealth_shell = type != nullptr && !ship.placement_preview
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "STEALTH"; });
    }
    // #614: a squadron a hangar launches later flies in a launch slot, which the first pass
    // did not see; without the clips the slot's craft never plays DEPLOY or UNDEPLOY.
    prepare_sfoil_clips();
    // #424: the setup's squadrons select and order as one unit, their team container.
    if (setup_) {
        for (const tactical::Squadron& squadron : setup_->squadrons) register_squadron(squadron, 0);
    }
    for (const ScriptedInput& input : options_.inputs) {
        // #518: an icon may be a squadron a spawner launches later, which the start does not hold;
        // a target that never shows is logged as not on screen.
        if (input.unit && !input.icon && !ship_of_entity_.contains(*input.unit)) {
            failure = "--eawr-live-input: unit " + std::to_string(*input.unit) + " is not a unit of the start";
            return false;
        }
    }
    // --eawr-live-late-orders: a unit the start does not hold (bought or launched later) has an ID
    // above every start unit's; an order on one is given as the local player, whose purchases they are.
    sim::EntityId last_start_unit = 0;
    for (const auto& [entity, ship] : ship_of_entity_) last_start_unit = std::max(last_start_unit, entity);
    for (const ScheduledOrder& order : options_.orders) {
        const auto late = [&](const sim::EntityId id) {
            return ship_of_entity_.contains(id) || (options_.late_orders && id > last_start_unit);
        };
        const auto stranger = std::find_if(order.more.begin(), order.more.end(),
            [&](const sim::EntityId id) { return !late(id); });
        if (!late(order.unit) || stranger != order.more.end()) {
            failure = "--eawr-live-order: unit " + std::to_string(stranger != order.more.end() ? *stranger : order.unit)
                + " is not a unit of the start";
            return false;
        }
        if (order.target != 0 && !ship_of_entity_.contains(order.target)) {
            failure = "--eawr-live-order: target " + std::to_string(order.target) + " is not a unit of the start";
            return false;
        }
    }
    // A driven frame shows the tick frame * step; a capture tick between two frames would be
    // drawn at a later tick than its file name says (#316 review 3).
    if (options_.stall && options_.real_time) {
        failure = "--eawr-live-stall is for driven captures";
        return false;
    }
    if (!options_.real_time) {
        for (const std::uint64_t tick : options_.capture_ticks) {
            // Past the stall a frame shows its step's tick plus the skipped ticks.
            std::uint64_t stepped = tick;
            if (options_.stall && (options_.stall->start || tick > options_.stall->tick)) {
                // The first frame presents tick <ticks> after a start stall.
                if (options_.stall->start ? tick < options_.stall->ticks
                                          : tick <= options_.stall->tick + options_.stall->ticks) {
                    failure = "--eawr-live-capture-ticks: tick " + std::to_string(tick) + " falls in the --eawr-live-stall skip";
                    return false;
                }
                stepped = tick - options_.stall->ticks;
            }
            const double frames = std::round(static_cast<double>(stepped) / options_.ticks_per_frame);
            if (std::abs(frames * options_.ticks_per_frame - static_cast<double>(stepped)) > capture_tolerance) {
                std::ostringstream message;
                message << "--eawr-live-capture-ticks: tick " << tick << " falls between frames at --eawr-live-step "
                        << options_.ticks_per_frame << "; a capture tick must be a multiple of the step";
                failure = message.str();
                return false;
            }
        }
    }
    return true;
}

std::shared_ptr<const LiveSessionView::PreparedBattle> LiveSessionView::prepared_battle() const {
    if (!setup_ || !content_ || !tables_) return {};
    return std::make_shared<const PreparedBattle>(PreparedBattle{
        {*setup_, 0, {}}, *content_, victory_, economy_, *tables_, start_});
}

bool LiveSessionView::start(std::string& failure) {
    core::load_profile::Scope load_scope(core::load_profile::Phase::session);
    if (!setup_ || !content_) {
        failure = "the live session was not prepared";
        return false;
    }
    platform::LiveSession::Options session_options;
    session_options.load_note_sink = [](std::span<const core::LoadNote> notes) {
        if (!notes.empty()) publish_viewer_load_notes({notes.begin(), notes.end()}, {}, true);
    };
    session_options.workers = options_.workers.value_or(platform::LiveSession::game_worker_count());
    session_options.measure_phases = trace_frames_;
    session_options.pacing = options_.real_time ? platform::LiveSession::Pacing::real_time
                                                : platform::LiveSession::Pacing::driven;
    session_options.target_rate = time_.target_rate(); // #459 TM-01
    session_options.initially_paused = true;
    // SND-40: only authored bases opt into the bounded durable presentation journal.
    for (const auto& type : tables_->units) {
        const auto base = [](const std::string& name) {
            const auto value = lower_path(name);
            return value == "dummy_star_base" || value == "dummy_orbital_structure";
        };
        const auto& behavior = type.footprint.hazard;
        if (std::any_of(behavior.behavior.begin(), behavior.behavior.end(), base)
            || std::any_of(behavior.space_behavior.begin(), behavior.space_behavior.end(), base)) {
            session_options.base_attack_types.push_back(battle::type_id(type.id));
        }
    }
    std::sort(session_options.base_attack_types.begin(), session_options.base_attack_types.end());
    // UI-07: the local player's one scheduler, taken on the simulation thread right before
    // each step with its own keys. The session starts at tick 0.
    // LAN assigns its agreed future tick after this handoff. A stalled admission
    // visit must not close the producer's current epoch (FD-01 phase order stays).
    std::function<std::uint64_t()> intent_epoch;
    if (options_.lan) intent_epoch = [this] { return session_ ? session_->completed_tick() : 0U; };
    else if (options_.borrowed_feed && options_.borrowed_feed->agreed_pacing())
        intent_epoch = [feed = std::weak_ptr(options_.borrowed_feed)] {
            const auto source = feed.lock(); return source ? source->completed_tick() : 0U;
        };
    scheduler_ = std::make_shared<ui::CommandScheduler>(player_, 0, 0, std::move(intent_epoch), setup_->controllers ? options_.controller_seat : sim::SeatId{});
    if (options_.lan) options_.lan->set_input_epoch_provider([scheduler = std::weak_ptr(scheduler_)] {
        const auto producer = scheduler.lock();
        return producer ? producer->pending_intent_epoch() : std::nullopt;
    });
    if (options_.lan) options_.lan->set_submission_observer([scheduler = std::weak_ptr(scheduler_)]
        (const std::uint64_t sequence, const std::uint64_t tick) {
        if (const auto producer = scheduler.lock()) producer->note_execution(sequence, tick);
    });
    if (options_.lan) options_.lan->set_discard_observer([scheduler = std::weak_ptr(scheduler_)](std::uint64_t sequence) {
        if (const auto producer = scheduler.lock()) producer->present_result(sequence);
    });
    if (options_.lan_acceptance && options_.lan) scheduler_->set_issue_observer(
        [lan = options_.lan](std::uint64_t sequence, std::uint64_t epoch) { lan->note_issue(sequence, epoch); });
    if (options_.lan) input_sink_ = std::make_unique<LanCommandSink>(*scheduler_, options_.lan,
        [this](const core::Diagnostic& error) { recovery_input_feedback_ = error.message; });
    order_input_ = std::make_unique<ui::OrderInput>(input_sink_ ? *input_sink_ : *scheduler_);
    session_options.scripts = ai_scripts_;
    if (options_.lan) session_options.step_driver = options_.lan->driver(*setup_);
    // #494: the local player's fog cells ride along with each tick for the fog in the world.
    if (content_->fog) session_options.fog_player = player_;
    session_options.command_source = [scheduler = scheduler_.get(), fault = options_.fault_tick](const std::uint64_t next_tick) {
        if (fault && next_tick == *fault) {
            throw std::runtime_error("--eawr-live-fault-tick injected a fault before tick " + std::to_string(next_tick));
        }
        return scheduler->take(next_tick);
    };
    if (options_.borrowed_feed) {
        session_ = options_.borrowed_feed;
        session_->configure_notifications(std::move(session_options.base_attack_types));
        if (session_->agreed_pacing()) {
            // The parent may continue while this view closes. Retain only the producer, with a weak epoch reader.
            session_->command_source([scheduler = scheduler_](const std::uint64_t next_tick) { return scheduler->take(next_tick); });
            session_->set_execution_observer([scheduler = std::weak_ptr(scheduler_)](
                const std::uint64_t sequence, const std::uint64_t tick) {
                if (const auto producer = scheduler.lock()) producer->note_execution(sequence, tick);
            });
        } else session_->command_source(std::move(session_options.command_source));
        return true;
    }
    tactical::TacticalContent content;
    content.sensors = content_->sensors;
    content.durability = content_->durability;
    content.motion = content_->motion;
    content.combat = content_->combat;
    content.abilities = content_->abilities;
    content.fog = content_->fog;
    content.map_box = content_->map_box;
    content.victory = victory_;
    content.economy = economy_;
    if (land_content_) content = *land_content_;
    content.load_notes.insert(content.load_notes.end(), content_->load_notes.begin(), content_->load_notes.end());
    if (replay_) {
        auto companion = options_.replay_input; companion += ".session";
        std::error_code code;
        const bool exists = std::filesystem::exists(companion, code);
        if (code) { failure = "Cannot inspect multiplayer replay session companion."; return false; }
        if (exists) {
            const auto size = std::filesystem::file_size(companion, code);
            if (code || size > net::match_pause_replay_limit) { failure = "Multiplayer session journal exceeds its byte limit."; return false; }
            std::ifstream input(companion, std::ios::binary);
            std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            auto journal = net::parse_match_pause_replay(bytes);
            if (!input || !journal) { failure = "Expected eawr-match-session-replay-v1 multiplayer journal."; return false; }
            auto offer = net::decode_setup_offer(journal.value().setup);
            auto encoded_setup = net::encode_skirmish_setup(replay_->setup);
            if (!offer || !encoded_setup || offer.value().domain != net::tactical_setup_domain
                || offer.value().setup != encoded_setup.value()
                || (journal.value().rows.empty() ? 0 : journal.value().rows.back().world_tick) != replay_->final_tick_count) {
                failure = "Multiplayer session journal and tactical replay setup or final world tick differ."; return false;
            }
            auto playback = std::make_shared<lan_detail::LivePauseReplay>(replay_->setup, offer.value().seats, std::move(journal).value());
            session_options.recorded_step_driver = [playback](platform::LiveStepContext& context,
                std::span<const tactical::PlayerCommand> commands, std::uint64_t) { return playback->step(context, commands); };
        }
    }
    auto live = replay_ ? platform::LiveSession::start(*replay_, content, session_options)
        : platform::LiveSession::start(*setup_, content, session_options);
    if (!live) {
        failure = "live session: " + core::format_diagnostic(live.error());
        return false;
    }
    if (land_content_) bind_land_roster(*live.value());
    session_ = std::make_shared<platform::StandaloneTacticalFeed>(std::move(live).value());
    // The debug hook: each order as its unit's owner would give it, at its tick.
    for (const ScheduledOrder& order : options_.orders) {
        const auto owner = owner_of_entity_.find(order.unit);
        platform::LiveOrder live_order{owner != owner_of_entity_.end() ? owner->second : player_, {order.unit},
                                       tactical::StopPayload{}, order.tick};
        live_order.units.insert(live_order.units.end(), order.more.begin(), order.more.end());
        if (order.kind == tactical::OrderKind::move) live_order.payload = tactical::MovePayload{order.point};
        if (order.kind == tactical::OrderKind::face) live_order.payload = tactical::FacePayload{order.point};
        if (order.kind == tactical::OrderKind::damage) {
            live_order.payload = tactical::DamagePayload{order.amount, order.hardpoint};
        }
        if (order.kind == tactical::OrderKind::attack) live_order.payload = tactical::AttackPayload{order.target};
        if (order.kind == tactical::OrderKind::ability) {
            tactical::AbilityPayload ability{order.ability, order.action};
            ability.target = order.target; // #561: none unless the order names one
            if ((order.ability == tactical::AbilityKind::fow_reveal_ping
                || order.ability == tactical::AbilityKind::weaken_enemy || order.ability == tactical::AbilityKind::buzz_droids
                    || order.ability == tactical::AbilityKind::corrupt_systems)
                && order.action == tactical::AbilityAction::activate)
                ability.position = order.point;
            live_order.payload = ability;
        }
        if (order.kind == tactical::OrderKind::attack_move) {
            live_order.payload = tactical::AttackMovePayload{order.point, order.target};
        }
        if (order.kind == tactical::OrderKind::guard) live_order.payload = tactical::GuardPayload{order.point, order.target};
        if (!session_->try_submit(std::move(live_order))) return false;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
