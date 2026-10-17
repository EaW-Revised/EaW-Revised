#include "galactic_mode_internal.hpp"
#include "campaign_loading_hud.hpp"

#include "eawr/campaign/conflict_entry.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/core/sha256.hpp"
#include "eawr/sim/tactical/content.hpp"
#include "eawr/platform/campaign_battle_view.hpp"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <chrono>
#include <fstream>

namespace eawr::presentation::godot_backend {
using namespace godot;

namespace {
std::uint64_t video_memory() {
    return static_cast<std::uint64_t>(Performance::get_singleton()->get_monitor(Performance::RENDER_VIDEO_MEM_USED));
}
}

GalacticMode::State::~State() {
    for (auto& particles : corruption_particles) particles.reset(); // Resources precede scene hosts.
    corruption_lines.reset(); corruption_paths.reset();
    corruption_uploaded_quads.clear(); corruption_quads.clear(); corruption_particle_layers.clear();
    save_controls.reset();
    if (retreat_layer) memdelete(retreat_layer);
    campaign_score.reset();
    battle_dialog.reset(); // Its root belongs to galaxy_container, deleted below.
    audio.reset();
    tactical.reset();
    if (tactical_host) memdelete(tactical_host);
    if (game) game->stop();
    if (loading) memdelete(loading);
    route_geometry.clear();
    capture_renderer.reset();
    renderer.reset();
    for (auto& layer : scenes) layer.renderer.reset();
    overlay.release();
    if (galaxy_container) memdelete(galaxy_container);
}

void GalacticMode::State::release_battle_presentation() {
    if (retreat_layer) memdelete(retreat_layer);
    if (retreat_hud) retreat_hud->set_retreat_handler({});
    retreat_layer = nullptr; retreat_root = nullptr; retreat_hud = nullptr;
    retreat_dialog.reset(); retreat_view.reset(); retreat_snapshot.reset(); retreat_dialog_open = false; retreat_completion_sent = false;
    if (tactical) tactical->close_requested();
    tactical.reset();
    if (tactical_host) memdelete(tactical_host);
    tactical_host = nullptr;
    if (loading) { loading->dismiss(); memdelete(loading); }
    loading = nullptr;
    prepared.reset();
    battle_content.reset();
    campaign_briefing.reset();
    conflict = 0;
    loading_frames = 0;
    if (galaxy_container) galaxy_container->show();
    if (galaxy_viewport) galaxy_viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    for (auto& layer : scenes) layer.viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    held.clear(); orbiting = false; scrolling = false;
    if (audio) audio->ambient(); // WGP-24: return without changing player pause.
}

void GalacticMode::State::sync_battle_viewport() {
    if (!host || (!loading && !tactical)) { battle_viewport = {}; return; }
    // Polled: the window's size_changed has no listener on the campaign path, and a load that blocks
    // the main loop queues the resize until a frame after it.
    const godot::Vector2 size = host->get_viewport()->get_visible_rect().size;
    if (size == battle_viewport) return;
    const bool seeded = battle_viewport.x > 0.0F && battle_viewport.y > 0.0F;
    battle_viewport = size;
    if (!seeded) {
        // Test hook: the window resizes once, while the loading screen is up (the GPU case for #3270/#3272).
        const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
        for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
            if (arguments[index] != String("--eawr-resize-in-loading")) continue;
            const PackedStringArray parts = arguments[index + 1].split("x");
            // A capture run pins the root viewport to its capture size; this run follows its window like play.
            host->get_window()->set_content_scale_mode(Window::CONTENT_SCALE_MODE_DISABLED);
            host->get_window()->set_content_scale_size(Vector2i(0, 0));
            if (parts.size() == 2 && parts[0].to_int() > 0 && parts[1].to_int() > 0)
                host->get_window()->set_size(Vector2i(static_cast<int32_t>(parts[0].to_int()),
                                                      static_cast<int32_t>(parts[1].to_int())));
            UtilityFunctions::print("EAWR resize in loading: window ", host->get_window()->get_size(),
                " viewport ", size, " scale mode ", static_cast<int>(host->get_window()->get_content_scale_mode()));
        }
        return; // the screen and the camera were built at this size
    }
    ++battle_viewport_syncs;
    battle_viewport_forwarded = size;
    UtilityFunctions::print("EAWR battle viewport forwarded: ", size);
    if (loading) loading->viewport_changed(size.x, size.y);
    if (tactical) tactical->viewport_changed(size.x, size.y);
}

void GalacticMode::State::capture_space_cycle_stage(std::string_view stage) {
    const auto directory = options.report_path.parent_path() / "space-cycle";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) { failure = "Cannot create space cycle capture directory"; return; }
    const auto path = directory / (std::string(stage) + ".png");
    const auto image = host->get_viewport()->get_texture()->get_image();
    if (image.is_null() || image->save_png(String(path.generic_string().c_str())) != OK) {
        failure = "Cannot capture space cycle stage " + std::string(stage);
        return;
    }
    proof_captures.push_back(path.generic_string());
    // Keep phase evidence beside the captures, including failed proof runs.
    const auto frame = game->frame();
    std::ofstream timeline(directory / (std::string(stage) + "-phase.json"));
    timeline << "{\"stage\":\"" << stage << "\",\"viewer_frame\":" << frames
             << ",\"session_tick\":" << frame.session_tick
             << ",\"tactical_tick\":" << (frame.battle ? frame.battle->completed_tick() : 0)
             << ",\"steady_milliseconds\":" << std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count() << "}\n";
}

bool GalacticMode::State::service_space_cycle_proof() {
    if (!game || !galaxy) { failure = "Space cycle proof has no campaign owner"; return false; }
    if (frames > 36000 && !proof_save_ready) { failure = "Space cycle proof did not finish within its frame budget"; return false; }
    const auto frame = game->frame();
    if (tactical && loading && loading->shown() && options.space_cycle_proof != "save") {
        if (loading->ready_for_begin()) loading->request_begin();
        return failure.empty();
    }
    if (!proof_move_submitted && frame.latest && frames >= 20) {
        const auto planets = frame.latest->planets();
        const auto fleets = frame.latest->fleets();
        if (proof_merge_target) {
            // Ordinary fleet commands settle in the native published snapshot;
            // never move a partial merge or manufacture a replacement roster.
            const auto target = std::find_if(fleets.begin(), fleets.end(),
                [&](const auto& row) { return row.id == proof_merge_target; });
            std::vector<sim::EntityId> members;
            if (target != fleets.end()) for (const auto& member : target->members) members.push_back(member.id);
            std::sort(members.begin(), members.end());
            if (members != proof_original_members) {
                if (frames > proof_merge_frame + 1000) {
                    failure = "Space cycle proof ordinary fleet merge did not publish its exact original members";
                    return false;
                }
                game->advance_to(frame.session_tick + 1);
                return true;
            }
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::MoveFleet{target->id, {target->orbit, proof_merge_destination}, {}}, {}});
            proof_move_submitted = true;
            capture_space_cycle_stage("galaxy");
        }
        for (const auto& fleet : fleets) {
            if (proof_move_submitted) break;
            if (fleet.owner != galaxy->local.player || fleet.members.empty() || fleet.motion.transit) continue;
            for (const auto& route : frame.latest->routes()) {
                if (route.from != fleet.orbit && route.to != fleet.orbit) continue;
                const auto destination = route.from == fleet.orbit ? route.to : route.from;
                const auto planet = std::find_if(planets.begin(), planets.end(),
                    [&](const auto& row) { return row.id == destination; });
                if (planet == planets.end() || planet->owner == fleet.owner || planet->owner == neutral_owner) continue;
                // Existing opposing orbital forces make this a played space
                // conflict rather than an unopposed ownership capture.
                if (std::none_of(fleets.begin(), fleets.end(), [&](const auto& other) {
                    return other.owner != fleet.owner && other.owner != neutral_owner
                        && other.orbit == destination && !other.members.empty();
                })) continue;
                if (options.space_cycle_proof == "win" || options.space_cycle_proof == "retreat") {
                    proof_merge_target = fleet.id;
                    proof_merge_destination = destination;
                    proof_merge_frame = frames;
                    for (const auto& other : fleets) {
                        if (other.owner != fleet.owner || other.orbit != fleet.orbit || other.motion.transit) continue;
                        for (const auto& member : other.members) proof_original_members.push_back(member.id);
                        if (other.id == fleet.id) continue;
                        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                            sim::galactic::MergeFleets{other.id, fleet.id}, {}});
                        ++proof_merge_orders;
                    }
                    std::sort(proof_original_members.begin(), proof_original_members.end());
                    game->advance_to(frame.session_tick + 1);
                    return true;
                }
                game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                    sim::galactic::MoveFleet{fleet.id, {fleet.orbit, destination}, {}}, {}});
                proof_move_submitted = true;
                for (const auto& member : fleet.members) proof_original_members.push_back(member.id);
                capture_space_cycle_stage("galaxy");
                break;
            }
            if (proof_move_submitted) break;
        }
        if (!proof_move_submitted) { failure = "Space cycle proof found no hostile authored fleet route"; return false; }
    }
    if (!proof_fight_enabled && frame.pending_conflict && frame.pending_conflict->choice_ready && battle_dialog
        && battle_dialog->view().phase == galactic_view::BattleDialogPhase::choice
        && battle_dialog->view().choice_ready
        && battle_dialog->view().conflict == frame.pending_conflict->conflict.id) {
        if (!battle_dialog->view().tactical_available || !battle_dialog->view().fight_allowed) {
            failure = "Space cycle proof Fight is unavailable: " + frame.tactical_refusal;
            return false;
        }
        proof_fight_enabled = true;
        capture_space_cycle_stage("fight");
        battle_dialog->activate(galactic_view::BattleChoice::fight);
    }
    // Save/Load owns the next input after the same real Move/Fight/map path.
    // Leave the ordinary live clock and native controls running, including
    // across replacement, without issuing combat orders or acknowledging results.
    if (options.space_cycle_proof == "save" && (tactical || proof_save_ready)) {
        if (!proof_save_ready && frame.conflict) {
            proof_save_ready = true;
            proof_map = frame.conflict->map_identity;
            capture_space_cycle_stage("tactical");
            UtilityFunctions::print("EAWR campaign save proof: map ready conflict=", conflict);
        }
        return failure.empty();
    }
    if (tactical && options.space_cycle_proof == "retreat" && retreat_view) {
        using namespace sim::tactical;
        retreat_dialog_open = true;
        if (!proof_retreat_requested && retreat_view->refusal == RetreatRefusal::none) {
            if (retreat_hud) {
                proof_retreat_button_enabled = retreat_hud->retreat().enabled;
                proof_retreat_button_point = retreat_hud->control_point("b_retreat");
            }
            request_retreat(RetreatAction::start);
            proof_retreat_requested = true;
            proof_map = frame.conflict->map_identity;
            capture_space_cycle_stage("tactical");
        }
        if (proof_retreat_requested && retreat_view->state.phase == RetreatPhase::countdown && !proof_retreat_cancel_requested) {
            if (!proof_retreat_countdown_frame) proof_retreat_countdown_frame = frames;
            // Capture the published phase after its dialog has rendered.
            if (frames >= proof_retreat_countdown_frame + 2) {
                proof_retreat_started = true;
                proof_retreat_button_flashing = retreat_hud && retreat_hud->retreat().flashing;
                capture_space_cycle_stage("countdown");
                request_retreat(RetreatAction::cancel);
                proof_retreat_cancel_requested = true;
            }
        }
        if (proof_retreat_cancel_requested && retreat_view->state.phase == RetreatPhase::idle && !proof_retreat_cancelled) {
            if (!proof_retreat_idle_frame) proof_retreat_idle_frame = frames;
            if (frames >= proof_retreat_idle_frame + 2) {
                proof_retreat_cancelled = true;
                proof_retreat_button_idle = retreat_hud && retreat_hud->retreat().enabled && !retreat_hud->retreat().flashing;
                capture_space_cycle_stage("cancelled");
            }
        }
        if (proof_retreat_cancelled && !proof_retreat_restarted && retreat_view->refusal == RetreatRefusal::none) {
            request_retreat(RetreatAction::start);
            proof_retreat_restarted = true;
        }
    }
    if (tactical && (options.space_cycle_proof == "win" || options.space_cycle_proof == "loss") && frame.conflict && frame.battle
        && (!proof_attack_issued || frame.battle->completed_tick() >= proof_attack_tick + 30)) {
        const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat});
        const auto binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
            [&](const auto& seat) { return seat.seat == sim::SeatId{galaxy->local.seat}; });
        if (feed && binding != frame.conflict->seats.end()) {
            std::vector<sim::EntityId> own;
            const auto visible = frame.battle->visible_entities(binding->player);
            const auto squadrons = frame.battle->squadrons();
            const auto commandable = [&](const auto& unit) {
                bool container = false;
                for (const auto& squadron : squadrons) {
                    if (squadron.container == unit.entity_id) container = true;
                    else if (std::find(squadron.members.begin(), squadron.members.end(), unit.entity_id)
                        != squadron.members.end()) return false;
                }
                return container || (unit.durability && unit.durability->hull > sim::math::Fixed{});
            };
            const sim::tactical::TacticalInstance* target = nullptr;
            const sim::tactical::TacticalInstance* approach = nullptr;
            for (const auto& unit : frame.battle->instances()) {
                if (!commandable(unit)) continue;
                if (unit.owner == binding->player) own.push_back(unit.entity_id);
                else if (sim::tactical::players_hostile(frame.battle->players(), binding->player, unit.owner)) {
                    if (!approach) approach = &unit;
                    if (std::find(visible.begin(), visible.end(), unit.entity_id) != visible.end()
                        && (!target || unit.entity_id == proof_attack_target)) target = &unit;
                }
            }
            std::sort(own.begin(), own.end());
            if (!own.empty() && target && proof_attack_target != target->entity_id) {
                // WST-11: ordinary direct attack refuses fog-hidden targets.
                // Keep an admitted order until its target changes, like player input.
                feed->submit({binding->player, std::move(own), sim::tactical::AttackPayload{target->entity_id}, {}});
                proof_attack_target = target->entity_id; proof_approach_target = 0;
                UtilityFunctions::print("EAWR campaign proof: attack visible target=", target->entity_id,
                    " tick=", frame.battle->completed_tick());
            } else if (!own.empty() && !target && approach && proof_approach_target != approach->entity_id) {
                // The fixture's authored opponent supplies the approach point;
                // attack-move reaches it through normal movement and fog admission.
                const auto& rows = approach->fixed_transform.rows;
                feed->submit({binding->player, std::move(own),
                    sim::tactical::AttackMovePayload{{rows[0][3], rows[1][3], rows[2][3]}}, {}});
                proof_approach_target = approach->entity_id; proof_attack_target = 0;
                UtilityFunctions::print("EAWR campaign proof: approach hidden opponent=", approach->entity_id,
                    " tick=", frame.battle->completed_tick());
            }
            if (!proof_attack_issued) { proof_map = frame.conflict->map_identity; capture_space_cycle_stage("tactical"); }
            proof_attack_issued = true;
            proof_attack_tick = frame.battle->completed_tick();
        }
    }
    if (battle_dialog && battle_dialog->view().phase == galactic_view::BattleDialogPhase::summary && !proof_summary_seen) {
        proof_summary_seen = true;
        proof_won = battle_dialog->view().won;
        proof_summary_milliseconds = battle_dialog->view().results.elapsed_milliseconds;
        proof_losses = battle_dialog->view().results.totals;
        proof_invasion_offered = battle_dialog->view().invasion_fleet != 0;
        if (frame.result && frame.latest) {
            proof_survivors_preserved = true;
            for (const auto id : proof_original_members) {
                const auto outcome = std::find_if(frame.result->forces.begin(), frame.result->forces.end(),
                    [&](const auto& row) { return row.id == id; });
                if (outcome != frame.result->forces.end() && outcome->hero_defeated) continue;
                if (outcome != frame.result->forces.end()
                    && outcome->disposition != campaign::ForceDisposition::survived
                    && outcome->disposition != campaign::ForceDisposition::retreated) continue;
                ++proof_survivors;
                const bool retained = std::any_of(frame.latest->fleets().begin(), frame.latest->fleets().end(),
                    [&](const auto& fleet) {
                        return std::any_of(fleet.members.begin(), fleet.members.end(),
                            [&](const auto& member) { return member.id == id; });
                    });
                proof_survivors_preserved = proof_survivors_preserved && retained;
            }
        }
        capture_space_cycle_stage("summary");
    }
    if (!cycles.empty() && proof_summary_seen && !proof_summary_acknowledged && battle_dialog) {
        battle_dialog->activate(galactic_view::BattleChoice::continue_galaxy);
        proof_summary_acknowledged = true;
    }
    game->advance_to(frame.session_tick + 1);
    return failure.empty();
}

bool GalacticMode::State::process_battle_transition(double delta) {
    if (!game || !galaxy) return false;
    sync_battle_viewport();
    const auto frame = game->frame();
    if (const auto error = game->failure()) {
        failure = core::format_diagnostic(*error);
        release_battle_presentation();
        return false;
    }
    if (conflict && frame.result && frame.result->conflict == conflict) {
        release_battle_presentation();
        if (!refresh_game()) return false;
        apply_camera();
        submit();
        returned = true;
        returned_frame = frames;
        returned_at = std::chrono::steady_clock::now();
        UtilityFunctions::print("EAWR campaign battle: returned conflict=", frame.result->conflict);
        return false;
    }
    if (!frame.conflict) {
        if (returned && frames - returned_frame >= 120
            && std::chrono::steady_clock::now() - returned_at >= std::chrono::seconds(3)) {
            return_memory = OS::get_singleton()->get_static_memory_usage();
            return_video_memory = video_memory();
            cycles.push_back({child_ready_tick, frame.result ? frame.result->tactical_frames : 0,
                galaxy_memory, combined_memory, return_memory, galaxy_video_memory, combined_video_memory,
                return_video_memory, battle_load_ms, child_lifetime.expired(), feed_lifetime.expired(),
                scene_lifetime.expired() && !UtilityFunctions::instance_from_id(static_cast<std::int64_t>(tactical_instance)),
                galaxy_host->get_instance_id() == galaxy_instance});
            returned = false;
        }
        // A restored owner can invalidate a retained feed before its replacement
        // publishes a child. D uses the same teardown hook before swapping.
        if (conflict) release_battle_presentation();
        return false;
    }
    const auto local_binding = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
        [&](const auto& seat) { return seat.seat == sim::SeatId{galaxy->local.seat}; });
    if (local_binding == frame.conflict->seats.end() || !local_binding->player) return false;
    if (frame.conflict->layer != campaign::BattleLayer::space) {
        // WGP-90/170: M3 has no playable land child. Auto-resolve stays with
        // its owner; an admitted tactical mission is not silently converted.
        failure = "Playable land conflict is unavailable: " + frame.conflict->map_identity;
        return false;
    }
    const auto preparation = game->loading(sim::SeatId{galaxy->local.seat});
    if (!conflict) {
        if (!filesystem || !audio_catalog) {
            failure = "Space battle presentation requires installed content";
            return false;
        }
        if (!loading && preparation) {
            auto mounted = std::make_shared<BattleContent>();
            mounted->filesystem = filesystem; mounted->catalog = audio_catalog;
            mounted->profile = profile; mounted->layers = layers;
            battle_content = std::move(mounted);
            loading = memnew(EawrLoadingScreen);
            auto load = campaign_loading_options(preparation, battle_content, content ? &*content : nullptr, [this, id = frame.conflict->id] {
                return (!conflict || conflict == id) && !retreat_dialog_open && (!save_controls || !save_controls->visible());
            });
            // WGP-51: clone the actual resident model and transform, independently
            // of the location fallback retained by the platform publication.
            const auto planet = static_cast<galactic_view::ObjectId>(preparation->descriptor.planet);
            if (const auto chosen = planet_models.find(planet); chosen != planet_models.end() && chosen->second) {
                for (const auto& instance : instances) {
                    const auto owner = instance_planets.find(instance.entity_id);
                    if (owner == instance_planets.end() || owner->second != planet) continue;
                    load.planet_model = models[*chosen->second].logical_path;
                    load.planet_transform = adapt_instance(instance).column_major;
                    break;
                }
            }
            if (!loading->build(*host, std::move(load))) {
                failure = "Cannot build campaign loading screen: " + loading->failure();
                release_battle_presentation(); return false;
            }
        }
        if (loading) loading->observe_preparation(preparation);
        if (preparation && preparation->outcome == platform::CampaignLoadingOutcome::failed) {
            failure = campaign_loading_error(preparation->descriptor,
                preparation->failure ? preparation->failure->message : "Preparation failed");
            if (loading) loading->show_failure(failure);
            return false;
        }
        if (!frame.battle_presentation) {
            if (preparation && preparation->outcome == platform::CampaignLoadingOutcome::pending) return true;
            failure = "Space battle presentation is unavailable for " + frame.conflict->map_identity;
            return false;
        }
        const auto& source = *frame.battle_presentation;
        auto battle = std::make_shared<LiveSessionView::PreparedBattle>();
        battle->replay.setup = source.setup;
        battle->content = source.content;
        battle->victory = source.victory;
        battle->economy = source.economy;
        battle->tables = source.tables;
        battle->start = source.start;
        for (const auto& type : source.tables.units)
            battle_tactical_types.emplace(skirmish::type_id(type.id), type.id);
        prepared = std::move(battle);
        if (!battle_content) {
            auto mounted = std::make_shared<BattleContent>();
            mounted->filesystem = filesystem; mounted->catalog = audio_catalog;
            mounted->profile = profile; mounted->layers = layers;
            battle_content = std::move(mounted);
        }
        presented_conflict = frame.conflict;
        conflict = frame.conflict->id;
        if (briefing_conflict != conflict) {
            briefing_conflict = conflict; briefing_done = briefing_accepted = intro_requested = false;
            briefing_movie.clear();
        }
        for (const auto& row : source.briefings) if (row.player == local_binding->player) {
            campaign_briefing = row;
            intro_requested = campaign::space_intro_allowed(row.intro);
            break;
        }
        returned = false;
        galaxy_instance = galaxy_host->get_instance_id();
        child_lifetime = frame.battle_lifetime;
        galaxy_memory = OS::get_singleton()->get_static_memory_usage();
        galaxy_video_memory = video_memory();
        held.clear(); orbiting = false; scrolling = false;
        galaxy_container->hide();
        galaxy_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
        for (auto& layer : scenes) layer.viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
        if (audio) audio->battle_load();
        if (!loading) {
            loading = memnew(EawrLoadingScreen);
            EawrLoadingScreen::Options load;
            load.content = battle_content;
            load.map_path = frame.conflict->map_identity;
            const auto planet = std::find_if(metadata.planets.begin(), metadata.planets.end(),
                [&](const auto& row) { return row.id == frame.conflict->planet; });
            if (planet != metadata.planets.end()) load.context_name = planet->name;
            load.map_name = frame.conflict->map_identity;
            load.campaign_space = true;
            load.topmost = [this, id = conflict] {
                return conflict == id && !retreat_dialog_open && (!save_controls || !save_controls->visible());
            };
            if (frame.latest) {
                const auto location = std::find_if(frame.latest->planets().begin(), frame.latest->planets().end(),
                    [&](const auto& row) { return row.id == frame.conflict->planet; });
                if (location != frame.latest->planets().end() && this->content)
                    if (const auto* owner = galactic_view::affiliation_for_owner(*this->content, location->owner)) {
                        load.owner_name = owner->faction;
                        load.owner_colour = {owner->secondary.r, owner->secondary.g, owner->secondary.b};
                    }
            }
            if (const auto income = basic_planet_income.find(frame.conflict->planet); income != basic_planet_income.end())
                load.basic_income = income->second;
            if (!loading->build(*host, std::move(load))) {
                failure = "Cannot load " + frame.conflict->map_identity + ": " + loading->failure();
                release_battle_presentation();
                return false;
            }
        }
        UtilityFunctions::print("EAWR campaign battle: loading conflict=", conflict);
        return true;
    }
    if (loading) loading->observe_preparation(preparation);
    if (!tactical) {
        if (++loading_frames < 3) {
            if (loading_frames == 2 && !options.space_cycle_proof.empty()) capture_space_cycle_stage("loading");
            return true;
        }
        const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat});
        const auto seat = std::find_if(frame.conflict->seats.begin(), frame.conflict->seats.end(),
            [&](const auto& binding) { return binding.seat == sim::SeatId{galaxy->local.seat}; });
        if (!feed || seat == frame.conflict->seats.end() || seat->player == 0) {
            failure = "Space battle has no playing feed for the local seat";
            release_battle_presentation();
            return false;
        }
        MapMode::Options map;
        map.game_root = options.game_root; map.mod_root = options.mod_root;
        map.map_path = frame.conflict->map_identity;
        const auto planet = std::find_if(metadata.planets.begin(), metadata.planets.end(),
            [&](const auto& row) { return row.id == frame.conflict->planet; });
        if (planet != metadata.planets.end()) map.campaign_planet = planet->name;
        map.populate = true; map.interactive = true;
        map.content = battle_content;
        map.loading = loading;
        // WGP-112/114: only solo campaign completion waits for player Begin.
        map.loading_begin_barrier = std::count_if(metadata.seats.begin(), metadata.seats.end(),
            [](const auto& binding) { return binding.active && binding.human; }) == 1;
        const bool begin_barrier = map.loading_begin_barrier;
        map.borrowed_battle = prepared; map.borrowed_feed = feed;
        map.borrowed_player = seat->player;
        map.report_path = options.report_path.parent_path() / "tactical.json";
        map.lighting = "sh"; map.shadows = "on"; map.environment = "map";
        feed_lifetime = feed;
        tactical_host = memnew(Node3D); host->add_child(tactical_host);
        tactical_instance = tactical_host->get_instance_id();
        tactical = std::make_shared<MapMode>(std::move(map));
        scene_lifetime = tactical;
        // The battle's options button opens the campaign's menu, the one Escape opens.
        tactical->set_menu_handler([this] { if (save_controls) save_controls->open_menu(); });
        const auto began = std::chrono::steady_clock::now();
        if (!tactical->ready(*tactical_host)) {
            const auto diagnostic = std::string(tactical->failure());
            failure = preparation ? campaign_loading_error(preparation->descriptor, diagnostic)
                : "Cannot prepare space battle map " + frame.conflict->map_identity + ": " + diagnostic;
            if (loading) loading->show_failure(failure);
            release_battle_presentation();
            return false;
        }
        if (!build_retreat_controls()) {
            release_battle_presentation();
            return false;
        }
        battle_load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        if (!begin_barrier) loading->dismiss();
        child_ready_tick = feed->completed_tick();
        game->battle_ready(conflict);
        tactical_host->set_name("CampaignSpaceBattleReady");
        if (audio) audio->tactical();
        UtilityFunctions::print("EAWR campaign battle: ready conflict=", conflict);
        return true;
    }
    if (loading && loading->shown() && loading->ready_for_begin()) {
        ++begin_wait_frames;
        if (frame.battle) begin_wait_last_tick = frame.battle->completed_tick();
    }
    if (const auto ended = tactical->process(delta); ended && *ended != 0) {
        failure = "Space battle presentation failed on " + frame.conflict->map_identity;
        release_battle_presentation();
        return false;
    }
    combined_memory = std::max(combined_memory, OS::get_singleton()->get_static_memory_usage());
    combined_video_memory = std::max(combined_video_memory, video_memory());
    service_retreat_controls();
    if (campaign_briefing && frame.battle) {
        campaign::SpaceBriefingGate gate;
        gate.enabled = campaign_briefing->enabled;
        gate.logical_frame = frame.battle->completed_tick();
        gate.speech_queue_empty = tactical->briefing_queue_empty();
        // No cinematic player is installed: the authored intro decision is
        // recorded, and the ordinary scene proceeds without guessed playback.
        if (const auto request = campaign::service_space_briefing(briefing_done, gate,
            campaign_briefing->content, campaign_briefing->available_persistent_types)) {
            briefing_accepted = tactical->campaign_briefing(request->speech);
            if (briefing_accepted) briefing_movie = request->command_bar_movie;
        }
    }
    return true;
}

bool GalacticMode::State::prepare_handoff() {
    if (!filesystem) { failure = "synthetic battle requires installed content"; return false; }
    if (!audio_catalog) { failure = "synthetic battle requires its prepared XML catalog"; return false; }
    auto mounted_content = std::make_shared<BattleContent>();
    // The resident HUD and its texture cache keep borrowing this same VFS.
    mounted_content->filesystem = filesystem;
    mounted_content->catalog = audio_catalog;
    mounted_content->profile = profile;
    mounted_content->layers = layers;
    battle_content = mounted_content;
    LiveSessionView::Options source_options;
    source_options.fixture = "m2";
    source_options.ai = false;
    // Reuse the established map/start/content preparation; no second live owner.
    LiveSessionView source(source_options);
    if (!source.prepare(*mounted_content->filesystem, *mounted_content->catalog, skirmish::m2_fixture().map, failure)) return false;
    const auto original = source.prepared_battle();
    if (!original) { failure = "synthetic battle preparation produced no content"; return false; }
    auto fixture = skirmish::m2_fixture();
    fixture.pre_built_base = false;
    fixture.free_starting_units = false;
    fixture.victory_condition = sim::tactical::VictoryCondition::all_enemy_units_destroyed;
    const auto invader = galaxy->local.player;
    const auto other = std::find_if(galaxy_setup.players.begin(), galaxy_setup.players.end(),
        [&](const auto& player) { return player.player_id != invader && player.player_id != neutral_owner; });
    if (other == galaxy_setup.players.end()) { failure = "synthetic battle needs a second player"; return false; }
    const auto defender = other->player_id;
    for (auto& slot : fixture.slots) {
        const auto source_owner = slot.slot;
        slot.slot = source_owner == 1 ? invader : defender;
        slot.colour_index = source_owner - 1;
    }
    std::sort(fixture.slots.begin(), fixture.slots.end(), [](const auto& a, const auto& b) { return a.slot < b.slot; });
    auto inputs = skirmish::read_start_inputs(fixture, *mounted_content->filesystem, *mounted_content->catalog, original->tables);
    if (!inputs) { failure = core::format_diagnostic(inputs.error()); return false; }

    galaxy_setup.content_identity = original->replay.setup.content_identity;
    const std::string name = "viewer-synthetic-handoff-v1";
    galaxy_setup.campaign_identity = core::sha256(std::span(reinterpret_cast<const std::uint8_t*>(name.data()), name.size()));
    sim::EntityId highest = 0;
    for (const auto& planet : galaxy_setup.planets) highest = std::max(highest, planet.id);
    for (const auto& route : galaxy_setup.routes) highest = std::max(highest, route.id);
    for (const auto& fleet : galaxy_setup.fleets) {
        highest = std::max(highest, fleet.id);
        for (const auto& member : fleet.members) highest = std::max(highest, member.id);
    }
    if (galaxy_setup.fleet_control) for (const auto& unit : galaxy_setup.fleet_control->detached_units)
        highest = std::max(highest, unit.member.id);
    if (galaxy_setup.production) for (const auto& planet : galaxy_setup.production->planets)
        for (const auto& object : planet.objects) highest = std::max(highest, object.id);
    if (galaxy_setup.census) for (const auto& object : galaxy_setup.census->objects)
        highest = std::max(highest, object.id);
    if (galaxy_setup.movement) for (const auto& container : galaxy_setup.movement->contents)
        for (const auto& child : container.children) highest = std::max(highest, child.id);
    if (galaxy_setup.control) for (const auto& planet : galaxy_setup.control->planets)
        for (const auto& force : planet.landed) highest = std::max(highest, force.id);
    platform::LiveGameSession::Options live_options;
    std::uint64_t entry_tick = 1;
    for (std::uint32_t index = 0; index < options.battle_cycles; ++index) {
        auto battle = std::make_shared<LiveSessionView::PreparedBattle>(*original);
        campaign::ConflictRecord entry;
        entry.id = ++highest;
        entry.planet = galaxy_setup.planets.front().id;
        entry.invader = invader;
        entry.defender = defender;
        entry.map_identity = fixture.map;
        std::vector<campaign::ForcePlacement> placements;
        for (const auto owner : {1U, 2U}) {
            const auto ship = std::find_if(original->start->units.begin(), original->start->units.end(), [owner](const auto& unit) {
                return unit.state.owner == owner && unit.role == skirmish::UnitRole::fleet && unit.craft == 0;
            });
            if (ship == original->start->units.end()) { failure = "synthetic conflict needs a fleet ship for each side"; return false; }
            const auto id = ++highest;
            entry.forces.push_back({id, ship->state.type_id, owner == 1 ? invader : defender, 0, campaign::ForceRole::escort});
            placements.push_back({id, ship->record, false});
        }
        for (const auto& seat : galaxy_setup.seats) if (seat.player == invader || seat.player == defender) entry.seats.push_back({seat.seat_id, seat.player});
        auto built = campaign::build_conflict_entry(entry, fixture, inputs.value(), placements, highest + 1, galaxy_setup.seed);
        if (!built) { failure = core::format_diagnostic(built.error()); return false; }
        auto selected = std::move(built).value();
        entry = std::move(selected.conflict);
        auto tactical_content = std::move(selected.content);
        battle->replay.setup = entry.tactical;
        battle->start = std::move(selected.start);
        battle->content = {tactical_content.sensors, tactical_content.durability, tactical_content.motion,
            tactical_content.combat, tactical_content.abilities, tactical_content.fog};
        battle->victory = tactical_content.victory;
        battle->economy = tactical_content.economy;
        battles.push_back(battle);
        live_options.entries.push_back({entry_tick, entry, tactical_content});
        entry_ticks.push_back(entry_tick);
        highest = *entry.tactical.first_free_entity_id - 1;
        entry_tick += options.battle_frames + tactical_content.victory.countdown_frames + 10;
    }
    prepared = battles.front();
    // This explicit fixture replaces the content identity on its initialized
    // layout. Ordinary campaigns transfer their retained root in start_game.
    campaign_root.reset();
    if (!start_game(std::move(live_options))) return false;
    galaxy_instance = galaxy_host->get_instance_id();
    UtilityFunctions::print("EAWR handoff phase: galaxy-resident cycle=1");
    return true;
}

bool GalacticMode::State::process_handoff(double delta) {
    sync_battle_viewport();
    if (returned) {
        // The capture harness returns through the same summary action as play.
        // Leave the settling frames below to present the uncovered galaxy.
        if (!interactive && battle_dialog
            && battle_dialog->view().phase == galactic_view::BattleDialogPhase::summary)
            battle_dialog->activate(galactic_view::BattleChoice::continue_galaxy);
        return_memory = OS::get_singleton()->get_static_memory_usage();
        return_video_memory = video_memory();
        if (frames - returned_frame < 120 || std::chrono::steady_clock::now() - returned_at < std::chrono::seconds(3))
            return true; // let deferred GPU destruction and process sampling settle
        if (cycles.size() == cycle) {
            cycles.push_back({child_ready_tick, game->frame().result->tactical_frames,
                galaxy_memory, combined_memory, return_memory, galaxy_video_memory,
                combined_video_memory, return_video_memory, battle_load_ms,
                child_lifetime.expired(), feed_lifetime.expired(), scene_lifetime.expired()
                    && !UtilityFunctions::instance_from_id(static_cast<std::int64_t>(tactical_instance)),
                galaxy_host->get_instance_id() == galaxy_instance});
            const auto& receipt = cycles.back();
            if (!receipt.child_expired || !receipt.feed_expired || !receipt.scene_expired || !receipt.galaxy_same) {
                failure = "handoff lifetime contract failed";
                return false;
            }
            UtilityFunctions::print("EAWR handoff phase: settled-galaxy cycle=", cycle + 1);
        }
        if (cycle + 1 < battles.size()) {
            ++cycle;
            prepared = battles[cycle];
            returned = false;
            conflict = 0;
            loading_frames = 0;
            combined_memory = combined_video_memory = 0;
            UtilityFunctions::print("EAWR handoff phase: galaxy-resident cycle=", cycle + 1);
        } else {
            if (!interactive) {
                if (!refresh_game()) return false;
                game->stop();
                game.reset();
                owner_expired = owner_lifetime.expired();
                if (!owner_expired) failure = "campaign owner retained after teardown";
            }
            return false;
        }
    }
    if (const auto error = game->failure()) { failure = core::format_diagnostic(*error); return false; }
    if (frames < 20) return true; // let the resident galaxy upload and present
    if (!conflict) {
        // The deterministic capture harness may be paused for art/lifetime checks.
        // A due, unchanged speed command services one parent boundary without resuming it.
        if (game->frame().latest->clock().pause_depth != 0 && game->completed_tick() <= entry_ticks[cycle])
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::SetSpeed{
                false, game->frame().latest->clock().strategic_speed}, {}});
        game->advance_to(entry_ticks[cycle] + 1);
        const auto frame = game->frame();
        if (!frame.conflict) return true;
        conflict = frame.conflict->id;
        if (audio) audio->battle_load(); // WGP-26: session conflict enters its loading screen.
        galaxy_memory = OS::get_singleton()->get_static_memory_usage();
        galaxy_video_memory = video_memory();
        UtilityFunctions::print("EAWR handoff phase: loading cycle=", cycle + 1);
        if (!refresh_game()) return false;
        galaxy_container->hide();
        galaxy_viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
        for (auto& layer : scenes) layer.viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
        held.clear();
        orbiting = false;
        loading = memnew(EawrLoadingScreen);
        EawrLoadingScreen::Options load;
        load.content = battle_content;
        load.map_path = frame.conflict->map_identity;
        load.context_name = "Coruscant";
        load.map_name = "Synthetic space battle";
        if (!loading->build(*host, std::move(load))) { failure = loading->failure(); return false; }
        return true;
    }
    if (!tactical) {
        if (++loading_frames < 3) return true; // show the loading screen before blocking preparation
        const auto feed = game->battle_feed(conflict, sim::SeatId{galaxy->local.seat});
        if (!feed) { failure = "local seat has no conflict-scoped battle feed"; return false; }
        // The foundation conflict owns its own player IDs. A loaded campaign's
        // display player can differ; use the same seat binding as battle_feed.
        const auto entry = game->frame().conflict;
        if (!entry || entry->id != conflict) { failure = "battle seat binding is stale"; return false; }
        const auto seat = std::find_if(entry->seats.begin(), entry->seats.end(),
            [&](const auto& binding) { return binding.seat == sim::SeatId{galaxy->local.seat}; });
        if (seat == entry->seats.end() || seat->player == 0) {
            failure = "synthetic battle requires a playing conflict seat";
            return false;
        }
        const auto battle_player = seat->player;
        MapMode::Options map;
        map.game_root = options.game_root;
        map.mod_root = options.mod_root;
        map.map_path = skirmish::m2_fixture().map;
        map.populate = true;
        map.interactive = true;
        map.content = battle_content;
        // The handoff owns its loading screen. MapMode's externally supplied
        // screen requires a manual Begin, while this transition begins on ready.
        map.borrowed_battle = prepared;
        map.borrowed_feed = feed;
        map.borrowed_player = battle_player;
        map.report_path = options.report_path.parent_path() / "tactical.json";
        map.lighting = "sh";
        map.shadows = "on";
        map.environment = "map";
        child_lifetime = game->frame().battle_lifetime;
        feed_lifetime = feed;
        tactical_host = memnew(Node3D);
        host->add_child(tactical_host);
        tactical_instance = tactical_host->get_instance_id();
        tactical = std::make_shared<MapMode>(std::move(map));
        scene_lifetime = tactical;
        tactical->set_menu_handler([this] { if (save_controls) save_controls->open_menu(); });
        const auto began = std::chrono::steady_clock::now();
        if (!tactical->ready(*tactical_host)) { failure = "retained-child tactical presentation failed to prepare"; return false; }
        if (audio) audio->tactical();
        battle_load_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count();
        loading->dismiss();
        child_ready_tick = feed->completed_tick();
        combined_memory = OS::get_singleton()->get_static_memory_usage();
        combined_video_memory = video_memory();
        // This deterministic foundation harness ends its own side after a short
        // battle. The damage hook is recorded like every other test command.
        for (const auto& unit : prepared->replay.setup.units) if (unit.owner == battle_player)
            feed->submit({unit.owner, {unit.entity_id}, sim::tactical::DamagePayload{
                sim::math::Fixed::from_integer(1'000'000).value(), 0xffffffffU}, options.battle_frames});
        game->battle_ready(conflict);
        UtilityFunctions::print("EAWR handoff phase: tactical-ready cycle=", cycle + 1);
        return true;
    }
    const auto frame = game->frame();
    combined_memory = std::max(combined_memory, OS::get_singleton()->get_static_memory_usage());
    combined_video_memory = std::max(combined_video_memory, video_memory());
    if (frame.result && frame.result->conflict == conflict) {
        tactical->close_requested();
        tactical.reset();
        memdelete(tactical_host);
        tactical_host = nullptr;
        loading->dismiss();
        memdelete(loading);
        loading = nullptr;
        galaxy_container->show();
        galaxy_viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
        for (auto& layer : scenes) layer.viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
        if (!refresh_game()) return false;
        returned = true;
        if (audio) audio->ambient(); // WGP-24: return from the child, no forced restart.
        returned_frame = frames;
        returned_at = std::chrono::steady_clock::now();
        UtilityFunctions::print("EAWR handoff phase: returned-galaxy cycle=", cycle + 1);
        return_memory = OS::get_singleton()->get_static_memory_usage();
        return_video_memory = video_memory();
        warmup = 20;
        return true;
    }
    if (const auto ended = tactical->process(delta); ended && *ended != 0) {
        failure = "retained-child tactical presentation failed while running";
        return false;
    }
    game->advance_to(frame.session_tick + 1);
    return true;
}

} // namespace eawr::presentation::godot_backend
