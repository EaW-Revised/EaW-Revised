#include "live_battle_bounty.hpp"
#include "galactic_mode_internal.hpp"
#include "orbit_hud_fixture.hpp"
#include "capture_viewport.hpp"
#include "viewer_path.hpp"
#include "hud_clock_fixture.hpp"
#include "capture_hud_fixture.hpp"
#include "auto_summary_hint.hpp"
#include "weapon_action_live_fixture.hpp"
#include "live_battle_dialog_view.hpp"
#include "galactic_mode_seats.hpp"
#include "galactic_reveal.hpp"
#include "native_ai_flag.hpp"

#include "eawr/core/diagnostic.hpp"
#include "eawr/campaign/setup_plan.hpp"

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cmath>
#include <numbers>
#include <fstream>
#include <limits>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {

using namespace galactic_detail;

namespace {
// Project keyboard-pan binding; retail pan speed remains unverified.
constexpr float keyboard_pan_fraction = 0.8F;
constexpr double keyboard_pan_max_delta = 0.1;
}

platform::LiveGameFrame GalacticMode::State::battle_dialog_frame() const {
    if (!game) return {};
    auto frame = galaxy ? game->frame(sim::SeatId{galaxy->local.seat}) : game->frame();
    return galactic_view::frame_with_auto_summary(std::move(frame), restored_auto_summary);
}
bool GalacticMode::State::build_view() {
    if (options.native_ai_invalid) {
        failure = "--eawr-galactic-ai expects on or off";
        return false;
    }
    if (options.galactic_reveal_invalid) {
        failure = "--eawr-galactic-reveal expects on or off";
        return false;
    }
    if (options.proof_options_invalid || (options.all_ai_capture && (interactive || options.paused
        || options.coop_preview || options.synthetic_battle || options.restore_save
        || !options.space_cycle_proof.empty() || !options.queue_type.empty()
        || options.campaign == galactic_view::synthetic_campaign))) {
        failure = "all-AI proof requires an unpaused authored campaign capture without command fixtures or restore";
        return false;
    }
    if (options.seat_invalid) {
        failure = "--eawr-galactic-seat expects a seat ID from 1 to 4294967295";
        return false;
    }
    if (options.camera_options_invalid) {
        failure = "galactic focus and flight-ms options require unsigned integer values";
        return false;
    }
    if (options.battle_invalid) {
        failure = "--eawr-galactic-battle expects synthetic and --eawr-galactic-battle-frames expects 1..3600; --eawr-galactic-battle-cycles expects 1..8";
        return false;
    }
    if (!options.space_cycle_proof.empty() && (options.campaign == galactic_view::synthetic_campaign
        || options.synthetic_battle || options.paused || options.coop_preview
        || (options.space_cycle_proof != "win" && options.space_cycle_proof != "retreat"
            && options.space_cycle_proof != "save" && options.space_cycle_proof != "prompt-save"
            && options.space_cycle_proof != "loss"))) {
        failure = "Space cycle proof requires an unpaused single-player authored campaign and mode win, retreat, loss, save or prompt-save";
        return false;
    }
    galactic_view::GalaxySnapshotStub snapshot = galactic_view::synthetic_galaxy();
    if (options.campaign == galactic_view::synthetic_campaign)
        galactic_view::configure_orbit_scene_fixture(snapshot, options.hud_fixture);
    if (options.campaign == galactic_view::synthetic_campaign && (options.hud_fixture == "radar-locked"
        || options.hud_fixture == "corruption-locked"))
        map_input.gates.tutorial_locked = true; // Explicit runtime fixture, never live campaign permissions.
    galaxy_setup = galactic_view::synthetic_live_setup();
    if (options.campaign == galactic_view::synthetic_campaign && !options.synthetic_battle && !options.hud_fixture.empty())
        galactic_view::configure_synthetic_hud_clock(galaxy_setup);
    if (filesystem) {
        auto loaded = data::load_catalog_strict(*filesystem, profile == "remake" ? data::Profile::remake
            : (profile == "foc" ? data::Profile::foc : data::Profile::eaw));
        if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
        auto tables = data::galactic::load_galactic_data(*filesystem, loaded.value().catalog);
        if (!tables) { failure = core::format_diagnostic(tables.error()); return false; }
        const bool synthetic = options.campaign == galactic_view::synthetic_campaign;
        auto text = data::ui::load_language_text_database(*filesystem, "english");
        production_art = galactic_view::load_production_art(loaded.value().catalog, text ? &text.value() : nullptr);
        if (synthetic && tables.value().campaigns.empty()) { failure = "no campaign provides galactic art settings"; return false; }
        auto scene = galactic_view::load_scene_content(*filesystem, loaded.value().catalog, tables.value(),
            synthetic ? tables.value().campaigns.front().name : options.campaign);
        if (!scene) { failure = core::format_diagnostic(scene.error()); return false; }
        content.emplace(std::move(scene.value()));
        if (synthetic) { content->art.clear(); content->snapshot = snapshot; content->route_styles.clear(); }
        else {
            snapshot = content->snapshot;
            campaign::CampaignSetupOptions setup_options;
            if (options.all_ai_capture) setup_options.forced_seed = options.capture_seed.value_or(1);
            if (restore_campaign) setup_options.custom = restore_campaign->custom;
            else if (options.custom_economy) {
                const auto& economy = *options.custom_economy;
                setup_options.custom = campaign::CampaignCustomOptions{
                    economy.credits, economy.starting_tech, economy.maximum_tech, true};
            }
            const auto observer = galactic_view::configure_campaign_seats(snapshot, options.seat, options.coop_preview);
            if (!observer) { failure = "campaign requires an active human observer seat"; return false; }
            if (const auto* refusal = galactic_view::reveal_refusal(options.galactic_reveal,
                    static_cast<std::size_t>(std::count_if(snapshot.seats.begin(), snapshot.seats.end(),
                        [](const auto& seat) { return seat.human && seat.active; })))) {
                failure = refusal;
                return false;
            }
            for (const auto& player : snapshot.players) {
                setup_options.players.push_back({{player.id, player.id, 0}, player.faction, true});
                const auto* faction = tables.value().faction(player.faction);
                if (faction && faction->is_neutral.value_or(false)) neutral_owner = player.id;
            }
            for (const auto& seat : snapshot.seats) setup_options.seats.push_back({sim::SeatId{seat.id}, seat.player,
                seat.human && !options.all_ai_capture ? sim::SeatKind::human : sim::SeatKind::ai, seat.id, seat.active,
                seat.human && !options.all_ai_capture ? seat.player : 0});
            const auto types = units::galactic_type_table(loaded.value().catalog);
            std::vector<std::string> type_names;
            for (const auto& type : types.types) type_names.push_back(type.name);
            std::sort(type_names.begin(), type_names.end());
            sim::galactic::TypeId next_type = 0;
            for (const auto& name : type_names) {
                setup_options.types.push_back({++next_type, name});
                production_names.emplace(next_type, name);
            }
            // WGUI-72..74: without display traits an enemy group stays a bare count (no icon), so supply one per type
            // from its authored object (hero class, maximum health), as the local cards already know them.
            fleet_display_traits = galactic_view::live_fleet_display_traits(production_names, production_art);
            setup_options.local_observer = observer->player;
            // SK-42/WGEP-16: the preview selects the authored Normal difficulty.
            auto created = platform::PreparedLiveCampaign::create(filesystem, loaded.value().catalog,
                tables.value(), types, options.campaign, setup_options, difficulty, options.native_ai || options.all_ai_capture);
            if (!created) { failure = core::format_diagnostic(created.error()); return false; }
            campaign_root = std::move(created).value();
            native_ai_installed = campaign_root->native_ai_installed();
            // A restored save would start its counters mid-game; report whole-game activity only.
            ai_activity = native_ai_installed && !options.restore_save;
            capture_metrics.neutral_owner = static_cast<sim::galactic::PlayerId>(neutral_owner);
            const auto& planned = campaign_root->plan();
            if (!options.restore_save && native_ai_installed) {
                // Report what the runtime installed, not every non-neutral AI seat.
                std::string names;
                for (const auto& installed : campaign_root->native_ai_players()) {
                    const auto binding = std::find_if(planned.player_bindings().begin(), planned.player_bindings().end(),
                        [&](const auto& player) { return player.active && player.player.player_id == installed.player; });
                    if (binding == planned.player_bindings().end()) continue;
                    native_ai_players.push_back({installed.player, binding->faction, installed.player_type});
                    names += (names.empty() ? "" : ", ") + binding->faction + "=" + installed.player_type;
                }
                UtilityFunctions::print("EAWR galactic: native AI on for ", static_cast<int64_t>(native_ai_players.size()),
                    " AI players (", String::utf8(names.c_str()), ")");
            } else if (!options.restore_save) {
                UtilityFunctions::print("EAWR galactic: native AI off");
            }
            galaxy_setup = campaign_root->layout();
            for (const auto& location : planned.locations()) {
                if (!location.planet) continue;
                const auto* source = tables.value().planet(location.original_type);
                if (!source) continue;
                const auto display_value = [](const sim::math::Fixed value) {
                    return static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale);
                };
                if (source->planet_credit_value)
                    basic_planet_income.emplace(location.planet->id, display_value(*source->planet_credit_value));
                if (source->planet_destroyed_credit_value)
                    destroyed_planet_income.emplace(location.planet->id, display_value(*source->planet_destroyed_credit_value));
            }
            // WGS-01a: associate authored names with graph IDs; art receives private display
            // IDs above the graph frontier and is explicitly excluded from simulation state.
            auto next_art = (galactic_view::ObjectId{1} << 63);
            std::map<galactic_view::ObjectId, galactic_view::PlanetArt> remapped_art;
            std::map<galactic_view::ObjectId, galactic_view::ObjectId> remapped_planets;
            const auto equal_name = [](const std::string_view a, const std::string_view b) {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
                    if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
                    if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
                    return x == y;
                });
            };
            for (auto& planet : snapshot.planets) {
                const auto location = std::find_if(planned.locations().begin(), planned.locations().end(),
                    [&](const auto& row) { return equal_name(row.original_type, planet.name); });
                if (location == planned.locations().end()) { failure = "scene object has no campaign location"; return false; }
                const auto old_id = planet.id;
                planet.id = location->planet ? location->planet->id : next_art++;
                if (location->planet) remapped_planets.emplace(old_id, planet.id);
                if (!location->planet) scene_objects.push_back(planet.id);
                remapped_art.emplace(planet.id, content->art.at(old_id));
            }
            content->art = std::move(remapped_art);
            // Authored styles use the scene's display IDs; the live graph has its own IDs.
            std::map<galactic_view::ObjectId, std::string> remapped_styles;
            for (const auto& old_route : snapshot.routes) {
                if (!remapped_planets.contains(old_route.from) || !remapped_planets.contains(old_route.to)) continue;
                const auto from = remapped_planets.at(old_route.from);
                const auto to = remapped_planets.at(old_route.to);
                const auto route = std::find_if(galaxy_setup.routes.begin(), galaxy_setup.routes.end(),
                    [&](const auto& candidate) {
                        return !remapped_styles.contains(candidate.id)
                            && ((candidate.from == from && candidate.to == to)
                                || (candidate.from == to && candidate.to == from));
                    });
                if (route != galaxy_setup.routes.end()) {
                    const auto style = content->route_styles.find(old_route.id);
                    remapped_styles.emplace(route->id, style == content->route_styles.end() ? "DEFAULT" : style->second);
                }
            }
            content->route_styles = std::move(remapped_styles);
            snapshot.routes.clear();
            for (const auto& route : galaxy_setup.routes) snapshot.routes.push_back({route.id, route.from, route.to});
            content->snapshot = snapshot;
            content->diagnostics.push_back("live campaign initialized with authored starting forces and political control");
        }
        audio_catalog = std::make_shared<const data::Catalog>(std::move(loaded).value().catalog);
    } else if (options.campaign != galactic_view::synthetic_campaign) {
        failure = "a campaign scene requires installed game data";
        return false;
    }
    galactic_view::SeatId seat = options.seat;
    if (seat == 0) {
        for (const galactic_view::StubSeat& candidate : snapshot.seats) {
            if (candidate.human && candidate.active && (seat == 0 || candidate.id < seat)) seat = candidate.id;
        }
    }
    const auto first_publication = campaign_root && options.galactic_reveal
        ? galactic_view::reveal_snapshot(campaign_root->snapshot()) : campaign_root ? campaign_root->snapshot() : nullptr;
    auto built = campaign_root ? galactic_view::build_live_view(*first_publication,
        snapshot, seat, neutral_owner, scene_objects,
        {galactic_view::live_fleet_visibility(*first_publication), {}, options.all_ai_capture || options.galactic_reveal, options.galactic_reveal}) : galactic_view::build_view(snapshot, seat);
    if (!built.view) {
        failure = built.failure;
        return false;
    }
    metadata = std::move(snapshot);
    galaxy = std::move(built.view);
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::fleet_hud_fixture(options.hud_fixture)
        && !galactic_view::ground_fixture(options.hud_fixture)) {
        galactic_view::configure_fleet_hud_fixture(galaxy_setup, galaxy->local.player, options.hud_fixture,
            fleet_display_traits, fleet_visibility, production_names);
        // Explicit synthetic audio witnesses; live type names come from the campaign.
        if (options.hud_fixture.starts_with("fleet-drag-xwing")) {
            production_names[1000] = "Rebel_X-Wing_Squadron";
            galaxy_setup.fleets[0].members = options.hud_fixture == "fleet-drag-xwing-mixed"
                ? std::vector<sim::galactic::FleetMember>{{7001,1000},{7002,1002}}
                : std::vector<sim::galactic::FleetMember>{{7001,1000}};
            if (options.hud_fixture == "fleet-drag-xwing-roster" && audio_catalog) {
                const std::array<data::ValueOverride, 1> roster{{
                    {"Rebel_X-Wing_Squadron", "Squadron_Units", {"X-Wing", "Tartan_Patrol_Cruiser"}}}};
                auto overridden = data::with_overrides(*audio_catalog, roster);
                if (!overridden) { failure = core::format_diagnostic(overridden.error()); return false; }
                audio_catalog = std::make_shared<const data::Catalog>(std::move(overridden).value());
            }
        }
    }
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "fleet-drag-home") {
        // Explicit native Home producer fixture: no configured home, first leader at another world.
        galaxy_setup.fleets.front().orbit = 102;
        // The census owner requires the live production service, with a complete seat/planet table.
        auto& production = galaxy_setup.production.emplace();
        production.maximum_human_queue = 5;
        for (const auto& player : galaxy_setup.players) {
            const bool human = std::any_of(galaxy_setup.seats.begin(), galaxy_setup.seats.end(), [&](const auto& candidate) {
                return candidate.player == player.player_id && candidate.kind == sim::SeatKind::human;
            });
            sim::galactic::ProductionPlayer row; row.player = player.player_id; row.human = human;
            row.pay_as_you_go = !human; row.uses_population_cap = human;
            production.players.push_back(row);
        }
        for (const auto& planet : galaxy_setup.planets) {
            sim::galactic::PlanetProduction row; row.planet = planet.id; row.target_owner = planet.owner;
            row.maximum_space_level = 5; production.planets.push_back(row);
        }
        auto& census = galaxy_setup.census.emplace();
        for (const auto& type : galaxy_setup.fleet_control->types) census.types.push_back({type.type, {}, 0});
        for (const auto& player : galaxy_setup.players) census.players.push_back({player.player_id, 50, false, {}});
        for (const auto& fleet : galaxy_setup.fleets) for (const auto& member : fleet.members)
            census.objects.push_back({member.id, member.type, fleet.owner, fleet.orbit, fleet.id, false, false,
                member.type, 0, sim::galactic::ActorCombatRole::ordinary});
        std::sort(census.objects.begin(), census.objects.end(), [](const auto& left, const auto& right) { return left.id < right.id; });
        if (content) {
            content->home_types.erase(galaxy->local.player);
            content->leader_types[galaxy->local.player] = production_names.at(census.objects.front().current_type);
        }
    }
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::ground_fixture(options.hud_fixture))
        galactic_view::configure_ground_fixture(galaxy_setup,galaxy->local.player,options.hud_fixture,
            fleet_display_traits,fleet_visibility,production_names);
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::weapon_action_live_fixture(options.hud_fixture))
        galactic_view::configure_weapon_action_live_fixture(galaxy_setup, galaxy->local.player,
            fleet_display_traits, fleet_visibility, production_names, options.hud_fixture);
    if (options.campaign == galactic_view::synthetic_campaign
        && (options.hud_fixture == "production" || options.hud_fixture == "production-unlocks")) {
        galactic_view::configure_synthetic_hud_production(galaxy_setup, galaxy->local.player);
        if (options.hud_fixture == "production-unlocks") {
            // SND-68: explicit synthetic registered types, independent of the
            // currently legal offers. One completed research unlocks both.
            galaxy_setup.production->technology_types = {{8100, 2, true, true, {galaxy->local.player}},
                {8101, 0, true, true, {galaxy->local.player}}};
        }
        production_names = {{1000, "TIE_Scout_Squadron"}, {1001, "Empire_Star_Base_2"},
            {1002, "DS_Shield_Gen"}, {1003, "Imperial_Stormtrooper_Squad"}, {1004, "E_Ground_Barracks"}};
    }
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "capture-live") {
        galactic_view::configure_capture_hud_fixture(galaxy_setup, galaxy->local.player);
        production_names = {{1003, "Imperial_Stormtrooper_Squad"}, {1004, "E_Ground_Barracks"}};
    }
    if (content) {
        if (options.campaign == galactic_view::synthetic_campaign)
            galactic_view::configure_orbit_fixture(galaxy_setup, galaxy->local.player, options.hud_fixture,
                fleet_display_traits, fleet_visibility, production_names);
        for (auto& planet : galaxy->planets) {
            galactic_view::update_affiliation(affiliations[planet.id],
                galactic_view::affiliation_for_owner(*content, planet.owner));
            if (options.campaign != galactic_view::synthetic_campaign) {
                planet.colour = affiliations[planet.id].primary;
                if (!planet.owner) planet.owner_faction = affiliations[planet.id].faction;
            }
        }
    }
    return true;
}

bool GalacticMode::State::start_game(platform::LiveGameSession::Options live_options) {
    if (!configure_score_fixture(live_options)) return false;
    configure_save_repository(live_options, options);
    const bool transit_fixture = options.campaign == galactic_view::synthetic_campaign
        && options.hud_fixture == "fleet-transit";
    live_options.workers = platform::LiveSession::game_worker_count();
    live_options.pacing = interactive && !transit_fixture ? platform::LiveGameSession::Pacing::real_time
        : platform::LiveGameSession::Pacing::driven;
    auto started = campaign_root ? platform::LiveGameSession::start(std::move(campaign_root), std::move(live_options))
        : platform::LiveGameSession::start(galaxy_setup, std::move(live_options));
    if (!started) { failure = core::format_diagnostic(started.error()); return false; }
    game = std::move(started).value();
    campaign_root.reset();
    owner_lifetime = game->lifetime();
    if (options.all_ai_capture || ai_activity) {
        const auto initial = game->frame();
        if (initial.latest) capture_metrics.observe(initial, {});
    }
    if (options.paused) {
        request_pause(true);
        // Driven sessions admit input at their explicit target; tick zero is not paused yet.
        if (!interactive || transit_fixture) game->advance_to(1);
        if (!game->wait_for(1, std::chrono::seconds(5))) {
            failure = "initial galaxy pause was not published";
            return false;
        }
    }
    if (transit_fixture) {
        // Drawing fixture only: movement admission requires an unpaused galaxy.
        // Driven pacing publishes each command before freezing the transit view.
        const auto publish = [&](sim::galactic::CommandPayload payload) {
            const auto target = game->frame().session_tick + 1;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(payload), {}});
            game->advance_to(target);
            return game->wait_for(target, std::chrono::seconds(5));
        };
        // Fixture distance only: travel clear of the source planet's model so the
        // fleet model and its facing are visible (#3071).
        constexpr std::uint64_t transit_fixture_ticks = 50;
        const auto travel = [&] {
            const auto target = game->frame().session_tick + transit_fixture_ticks;
            game->advance_to(target);
            return game->wait_for(target, std::chrono::seconds(5));
        };
        if ((options.paused && !publish(sim::galactic::ResumeGalaxy{}))
            || !publish(sim::galactic::MoveFleet{601, {101,102}, std::nullopt}) || !travel()
            || (options.paused && !publish(sim::galactic::PauseGalaxy{}))) {
            failure = "fleet transit drawing fixture was not published";
            return false;
        }
        const auto frame = game->frame();
        if (!std::any_of(frame.latest->fleets().begin(), frame.latest->fleets().end(),
            [](const auto& fleet) { return fleet.id == 601 && fleet.motion.transit.has_value(); })) {
            failure = "fleet transit drawing fixture did not enter transit";
            return false;
        }
    }
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "capture-live") {
        const auto publish = [&](sim::galactic::CommandPayload payload) {
            const auto target = game->frame().session_tick + 1;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(payload), {}});
            game->advance_to(target);
            return game->wait_for(target, std::chrono::seconds(5));
        };
        if ((options.paused && !publish(sim::galactic::ResumeGalaxy{}))
            || !publish(sim::galactic::LandFleet{601, 101})) {
            failure = "live capture fixture landing failed"; return false;
        }
        const auto target = game->frame().session_tick + 35;
        game->advance_to(target);
        if (!game->wait_for(target, std::chrono::seconds(5))
            || (options.paused && !publish(sim::galactic::PauseGalaxy{}))) {
            failure = "live capture fixture control was not published"; return false;
        }
    }
    if (!run_score_ledger()) return false;
    if (!options.queue_type.empty() && !queue_requested_build()) return false;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::weapon_action_live_fixture(options.hud_fixture)) {
        // Live acceptance fixture: the ability menu is opened by a real seat command; the HUD then queues the host's
        // typed capability (never a copied menu). Real-time pacing processes it even while the galaxy is paused.
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::PrepareGalacticAbility{
            galactic_view::live_hack_actor, galactic_view::live_hack_planet,
            options.hud_fixture.starts_with("destruction-live") ? sim::galactic::AbilityKind::planet_destruction
                : sim::galactic::AbilityKind::superweapon_hack}, {}});
    }
    return refresh_game();
}

bool GalacticMode::State::queue_requested_build() {
    // Qualification hook: the ordinary purchase command, issued once for the local seat.
    if (!interactive) game->advance_to(1);
    if (!game->wait_for(1, std::chrono::seconds(5))) {
        failure = "queue hook: the first galaxy frame was not published";
        return false;
    }
    const auto frame = game->frame();
    if (!frame.latest) { failure = "queue hook: no published frame"; return false; }
    const auto same_name = [](std::string_view left, std::string_view right) {
        return std::equal(left.begin(), left.end(), right.begin(), right.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
        });
    };
    for (const auto& offer : frame.latest->production_offers()) {
        if (offer.player != galaxy->local.player
            || (options.queue_planet != 0 && offer.planet != options.queue_planet)) continue;
        const auto name = production_names.find(offer.type);
        if (name == production_names.end() || !same_name(name->second, options.queue_type)) continue;
        if (offer.admission != sim::galactic::ProductionAdmission::allowed) continue;
        const auto target = frame.session_tick + 1;
        // WGEP-97: the hook stands for the build-button submission; audio starts after setup.
        queued_underway = name->second;
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
            sim::galactic::ProductionCommand{offer.planet, offer.type, sim::galactic::ProductionAction::buy,
                offer.domain, 0}, {}});
        ++submitted_orders;
        if (!interactive) {
            game->advance_to(target);
            if (!game->wait_for(target, std::chrono::seconds(5))) {
                failure = "queue hook: the purchase was not published";
                return false;
            }
        }
        return true;
    }
    failure = "queue hook: no admitted offer of " + options.queue_type + " for the local player";
    return false;
}

sim::math::Fixed GalacticMode::State::strategic_speed_now() const {
    if (submitted_strategic_speed && submitted_speed_snapshot == displayed_snapshot) return *submitted_strategic_speed;
    return displayed_snapshot ? displayed_snapshot->clock().strategic_speed : sim::math::Fixed::from_raw(sim::math::Fixed::scale);
}

void GalacticMode::State::submit_strategic_speed(const sim::math::Fixed factor) {
    game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::SetSpeed{false, factor}, {}});
    ++submitted_orders;
    submitted_strategic_speed = factor;
    submitted_speed_snapshot = displayed_snapshot;
}

void GalacticMode::State::set_perf_overlay(const bool shown) {
    if (perf == nullptr) {
        if (!shown || host == nullptr) return;
        // Above the HUD (canvas layer 1) and the battle overlays, in the HUD's face (the engine font without a cache).
        auto* layer = memnew(CanvasLayer);
        layer->set_name("EawrPerfOverlayLayer");
        layer->set_layer(100);
        perf = memnew(EawrPerfOverlay);
        perf->set_name("EawrPerfOverlay");
        Ref<Font> font;
        std::string face = "engine font";
        if (hud_fonts) {
            const presentation::ui::ResolvedFont resolved = hud_fonts->resolve({"EmpireAtWar-Medium", 7}, "ENGLISH");
            font = hud_fonts->font(resolved);
            if (!resolved.face.empty()) face = resolved.face;
        }
        perf->setup(font, face);
        layer->add_child(perf);
        host->add_child(layer);
    }
    perf->set_shown(shown);
}

void GalacticMode::State::sync_perf_overlay() {
    if (perf == nullptr || !perf->shown()) return;
    std::vector<presentation::ui::PerfTick> ticks;
    if (game) {
        // After a hidden spell the old ticks are skipped, not drawn as one burst.
        const bool rebase = perf->wants_rebase();
        for (const platform::LiveTickCost& cost : game->tick_costs_after(perf_last_tick)) {
            perf_last_tick = cost.tick;
            if (rebase) continue;
            presentation::ui::PerfTick tick{cost.tick, cost.total_ms, {}};
            for (const platform::LivePhaseCost& phase : cost.phases) tick.phases.push_back({phase.name, phase.ms});
            ticks.push_back(std::move(tick));
        }
        if (displayed_snapshot) {
            const auto& clock = displayed_snapshot->clock();
            const double factor = static_cast<double>(clock.strategic_speed.raw()) / static_cast<double>(sim::math::Fixed::scale);
            perf->set_clock(factor, factor * static_cast<double>(sim::galactic::galactic_frames_per_second), clock.pause_depth != 0);
        }
    }
    perf->frame(ticks, std::nullopt, game != nullptr);
}

std::string GalacticMode::State::perf_report_json() const {
    if (perf != nullptr) return perf->report_json();
    return std::string("{\"key\": \"") + EawrPerfOverlay::key_name + "\", \"shown\": false}";
}

void GalacticMode::State::request_pause(const bool value) {
    if (!game || !pause_guard.begin(paused, value)) return;
    // WGM-05/06: input enters the session as a seat-issued nested pause/resume command.
    // TM-05: pausing from fast forward ends it first, so play resumes at the normal speed.
    // The displayed snapshot can lag a press in the same frame, so submit 1x whenever the factor exists (a no-op at 1x).
    if (value && fast_forward_factor) submit_strategic_speed(sim::math::Fixed::from_raw(sim::math::Fixed::scale));
    if (value) game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::PauseGalaxy{}, {}});
    else game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::ResumeGalaxy{}, {}});
    ++submitted_orders;
}

bool GalacticMode::State::refresh_game() {
    if (!game) return true;
    if (const auto error = game->failure()) { failure = core::format_diagnostic(*error); return false; }
    const auto frame = game->frame();
    if (!frame.latest) return true;
    if (options.all_ai_capture || ai_activity) {
        const auto events = game->galactic_events_after(capture_metrics.cursor);
        capture_metrics.observe(frame, events);
        if (options.all_ai_capture && !capture_metrics.failure.empty()) { failure = capture_metrics.failure; return false; }
    }
    if (frame.latest == published_snapshot) return true;
    // #3931: with the reveal on, every presentation reader gets the omniscient draw view of this publication.
    const auto shown = options.galactic_reveal ? galactic_view::reveal_snapshot(frame.latest) : frame.latest;
    if (!galactic_view::fleet_hud_fixture(options.hud_fixture))
        fleet_visibility = galactic_view::live_fleet_visibility(*shown);
    auto built = galactic_view::build_live_view(*shown, metadata, galaxy->local.seat, neutral_owner,
        scene_objects, {fleet_visibility, {}, options.all_ai_capture || options.galactic_reveal, options.galactic_reveal});
    if (!built.view) { failure = built.failure; return false; }
    const bool changed = !displayed_snapshot
        || galaxy->fleets != built.view->fleets
        || !std::equal(shown->planets().begin(), shown->planets().end(),
            displayed_snapshot->planets().begin(), displayed_snapshot->planets().end())
        || !std::equal(shown->routes().begin(), shown->routes().end(),
            displayed_snapshot->routes().begin(), displayed_snapshot->routes().end())
        || !std::equal(shown->fleets().begin(), shown->fleets().end(),
            displayed_snapshot->fleets().begin(), displayed_snapshot->fleets().end())
        || (shown->observations().data() != displayed_snapshot->observations().data()
            && !std::equal(shown->observations().begin(), shown->observations().end(),
                displayed_snapshot->observations().begin(), displayed_snapshot->observations().end()));
    galaxy = std::move(built.view);
    displayed_snapshot = shown;
    published_snapshot = frame.latest;
    ++snapshots_presented;
    paused = shown->clock().pause_depth != 0;
    snapshot_events.assign(shown->events().begin(), shown->events().end());
    // Pause commands nest. Admit one toggle until its accepted state is
    // published or the local seat receives a rejection; display stays factual.
    pause_guard.observe(paused, sim::SeatId{galaxy->local.seat}, snapshot_events);
    if (content && options.campaign != galactic_view::synthetic_campaign) {
        for (auto& planet : galaxy->planets) {
            galactic_view::update_affiliation(affiliations[planet.id], galactic_view::affiliation_for_owner(*content, planet.owner));
            planet.colour = affiliations[planet.id].primary;
            if (!planet.owner) planet.owner_faction = affiliations[planet.id].faction;
        }
    }
    // Tick-only publications never recreate GPU geometry or reload models.
    if (changed) {
        if (renderer && !refresh_model_instances()) return false;
        if (renderer && !build_overlay()) return false;
        // Reuse the route adapter's relation seam when live ownership changes.
        if (route_styles) {
            for (const auto& route : galaxy->routes) {
                const auto line = route_lines.find(route.id);
                if (line == route_lines.end()) continue;
                const auto from = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
                    [&](const auto& planet) { return planet.id == route.from; });
                const auto to = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
                    [&](const auto& planet) { return planet.id == route.to; });
                if (from == galaxy->planets.end() || to == galaxy->planets.end()) continue;
                const auto owners = route_owners(*from, *to);
                if (line->second.set_type(galactic_view::route_type(owners, options.red_green_routes,
                    route_styles->good_faction, route_styles->evil_faction)) && line->second.zoom() == 1.0F) {
                    // A recreated relation invalidates its focused zoom material/spark state.
                    static_cast<void>(line->second.set_zoom(0.0F));
                    static_cast<void>(line->second.set_zoom(1.0F));
                }
            }
        }
        if (renderer) submit();
    }
    update_hud();
    return true;
}

galactic_view::OrbitCamera& GalacticMode::State::seat_camera() {
    return cameras.try_emplace(galaxy->local.seat, overview).first->second;
}

bool GalacticMode::State::planet_discovered(const galactic_view::ObjectId id) const {
    return planet_discovered_in(displayed_snapshot, id);
}

bool GalacticMode::State::planet_discovered_in(const std::shared_ptr<const sim::galactic::GalacticSnapshot>& source,
    const galactic_view::ObjectId id) const {
    const auto fact = hud_facts.find(id);
    const bool synthetic = options.campaign == galactic_view::synthetic_campaign;
    const auto observed = source ? galactic_view::observer_discovery(*source, galaxy->local.player, id) : std::nullopt;
    return observed.value_or(fact == hud_facts.end() ? synthetic : fact->second.discovered);
}

std::vector<galactic_view::InputPlanet> GalacticMode::State::input_planets() const {
    std::vector<galactic_view::InputPlanet> result;
    if (!galaxy) return result;
    for (const auto& planet : galaxy->planets) {
        // Ignore scene-only art; discovery comes from the local faction's observer.
        if (std::find(scene_objects.begin(), scene_objects.end(), planet.id) != scene_objects.end()) continue;
        // Named campaigns require published discovery. The explicit synthetic
        // harness retains its fixture discovery when no observer is installed.
        // Command gating: discovery comes from the truth publication, never the #3931 reveal view.
        auto allegiance = planet.owner.value_or(0);
        if (displayed_snapshot && displayed_snapshot->control()) {
            const auto& control = displayed_snapshot->control()->state;
            const auto current = std::find_if(control.begin(), control.end(),
                [&](const auto& row) { return row.planet == planet.id; });
            if (current != control.end()) allegiance = current->current_player; // WGUI-75: current allegiance.
        }
        result.push_back({planet.id, planet.position, allegiance, planet_discovered_in(published_snapshot, planet.id)});
    }
    return result;
}

std::vector<galactic_view::InputPlanet> GalacticMode::State::selection_planets() const {
    auto result = input_planets();
    if (!options.galactic_reveal) return result;
    for (auto& planet : result) {
        const auto fact = hud_facts.find(planet.id);
        if (fact != hud_facts.end() && fact->second.discovered) planet.discovered = true;
    }
    return result;
}

void GalacticMode::State::apply_selection(const galactic_view::SelectionEffects& effects) {
    if (effects.tutorial) ++tutorial_notifications; // Tutorial callback producer seam.
    const auto target = std::find_if(galaxy->planets.begin(), galaxy->planets.end(),
        [&](const auto& p) { return p.id == effects.target; });
    if (effects.position && target != galaxy->planets.end()) seat_flight().position(target->position);
    if (effects.changed) {
        selected_planet = map_input.selected;
        ++selection_invalidations; // WGUI-48: invalidate display/build choices before refresh.
        const bool space_tab = selected_hud.space_tab;
        selected_hud = {};
        selected_hud.space_tab = space_tab;
        update_hud(); // Name/cursor/sound producers remain the HUD presentation seam.
    }
    if (target == galaxy->planets.end()) return;
    const auto model = planet_models.find(target->id);
    const bool renderable = model != planet_models.end() && model->second.has_value();
    if (effects.refocus) static_cast<void>(seat_flight().refocus(*target, renderable));
    if (effects.zoom) {
        if (effects.toggle_zoom) static_cast<void>(seat_flight().toggle(*target, renderable));
        else static_cast<void>(seat_flight().zoom_in(*target, renderable));
    }
    if (effects.refocus || effects.zoom) apply_camera(); // GHUDL-11: refresh even when selection is unchanged.
}

galactic_view::PlanetCamera& GalacticMode::State::seat_flight() {
    return flights.try_emplace(galaxy->local.seat, camera_constants).first->second;
}

const galactic_view::PlanetView* GalacticMode::State::picked_planet(const float x, const float y) const {
    // Project picking: project planet centres through the current camera; the
    // HUD's discovery/selection gates will replace this synthetic-world seam.
    const Vector3 eye(camera.eye[0], camera.eye[1], camera.eye[2]);
    const Vector3 target(camera.target[0], camera.target[1], camera.target[2]);
    const Vector3 forward = (target - eye).normalized();
    const Vector3 right = forward.cross(Vector3(0, 1, 0)).normalized();
    const Vector3 up = right.cross(forward);
    const float tangent = std::tan(camera.vertical_fov_degrees * std::numbers::pi_v<float> / 360.0F);
    const Vector3 ray = (forward + right * ((x - static_cast<float>(camera.width) * 0.5F)
        * 2.0F * tangent / static_cast<float>(camera.height)) - up * ((y - static_cast<float>(camera.height) * 0.5F)
        * 2.0F * tangent / static_cast<float>(camera.height))).normalized();
    const galactic_view::PlanetView* best = nullptr;
    float nearest = 24.0F * 24.0F;
    float nearest_ray = std::numeric_limits<float>::max();
    for (const auto& planet : galaxy->planets) {
        if (planet_layer(planet.id) != 0) continue;
        const auto point = galactic_view::render_point(planet.position);
        const Vector3 offset = Vector3(point[0], point[1], point[2]) - eye;
        const float depth = offset.dot(forward);
        if (depth <= camera.near_plane) continue;
        if (content && content->art.contains(planet.id)) {
            const float radius = content->art.at(planet.id).pick_radius;
            if (radius <= 0.0F) continue;
            const float along = offset.dot(ray);
            const float discriminant = radius * radius - (offset.length_squared() - along * along);
            if (discriminant < 0.0F) continue;
            const float hit = along - std::sqrt(discriminant);
            if (hit >= camera.near_plane && hit < nearest_ray) { nearest_ray = hit; best = &planet; }
            continue;
        }
        const float scale = static_cast<float>(camera.height) / (2.0F * depth * tangent);
        const float dx = static_cast<float>(camera.width) * 0.5F + offset.dot(right) * scale - x;
        const float dy = static_cast<float>(camera.height) * 0.5F - offset.dot(up) * scale - y;
        if (dx * dx + dy * dy < nearest) { nearest = dx * dx + dy * dy; best = &planet; }
    }
    return best;
}

void GalacticMode::State::apply_camera() {
    if (interactive) {
        const Vector2 size = host->get_viewport()->get_visible_rect().size;
        if (size.x >= 1.0F && size.y >= 1.0F) {
            camera.width = static_cast<std::uint32_t>(size.x);
            camera.height = static_cast<std::uint32_t>(size.y);
        }
    }
    seat_camera() = seat_flight().pose();
    camera = galactic_view::fixed_camera(seat_camera(), camera.width, camera.height,
        galactic_view::galactic_vertical_fov(camera.width, camera.height));
    const Vector2i viewport_size(static_cast<int32_t>(camera.width), static_cast<int32_t>(camera.height));
    // The stretched container owns its child's render resolution (#2153).
    galaxy_container->set_size(Vector2(viewport_size));
    renderer->set_camera(camera);
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        Layer& layer = scenes[index];
        const Vector2i size(static_cast<int32_t>(camera.width), static_cast<int32_t>(camera.height));
        if (layer.viewport->get_size() != size) layer.viewport->set_size(size);
        layer.image->set_size(Vector2(static_cast<float>(camera.width), static_cast<float>(camera.height)));
        if (index != 0) {
            FixedCamera layer_camera = camera;
            // WGP-01a in gc-presentation.md: the
            // shared nonworld render temporarily uses this depth interval.
            layer_camera.near_plane = 10.0F;
            layer_camera.far_plane = 100000.0F;
            layer.renderer->set_camera(layer_camera);
        }
    }
    capture_renderer->set_camera(camera);
    update_hud();
    const auto light = seat_flight().lighting();
    if (applied_light == light) return;
    auto environment = galactic_view::galaxy_lighting(light.angles);
    environment.lights[0].color.a = light.level;
    GodotRenderer::LightingState lighting_state;
    lighting_state.sph = lighting::source_to_render(lighting::sph_light_all(environment)).rgb;
    lighting_state.sph_fill = lighting::source_to_render(lighting::sph_light_fill(environment)).rgb;
    lighting_state.ambient = {environment.ambient.r * environment.ambient.a, environment.ambient.g * environment.ambient.a, environment.ambient.b * environment.ambient.a};
    lighting_state.sun_diffuse = lighting::sun_diffuse(environment);
    const auto sun = lighting::source_to_render(environment.lights[0].direction);
    lighting_state.toward_light = {-sun.x, -sun.y, -sun.z};
    lighting_state.specular = {0.0F, 0.0F, 0.0F};
    renderer->set_lighting(lighting_state);
    for (std::size_t index = 1; index < scenes.size(); ++index) scenes[index].renderer->set_lighting(lighting_state);
    const auto direction = galactic_view::render_point(galactic_view::source_light_direction(light.angles));
    for (const auto& model : models) {
        GodotRenderer& target = model.layer == 0 ? *renderer : *scenes[model.layer].renderer;
        for (const auto& surface : model.surfaces) {
            if (surface.status != "drawn" || surface.route != "planet") continue;
            const auto directed = target.set_material_vector3(surface.asset, "eawr_effect_light_direction", direction);
            const auto diffuse = target.set_material_vector3(surface.asset, "eawr_effect_diffuse",
                {light.level, light.level, light.level});
            if (!directed || !diffuse) failure = "the galactic planet light could not be updated";
        }
    }
    applied_light = light;
}

std::optional<std::array<float, 12>> GalacticMode::State::zoom_pose(const bool authored) const {
    const auto model = planet_models.find(options.zoom_probe_planet);
    if (model == planet_models.end() || !model->second) return std::nullopt;
    const bool has_planet_surface = std::any_of(models[*model->second].surfaces.begin(),
        models[*model->second].surfaces.end(), [](const auto& surface) {
            return surface.status == "drawn" && surface.route == "planet";
        });
    for (const auto& surface : models[*model->second].surfaces) {
        // Prefer the physical planet, never its leading additive glow. Models
        // without Planet.fx (asteroids / black hole) use their own surface.
        if (surface.status != "drawn" || (has_planet_surface && surface.route != "planet")
            || (surface.alternate >= 0 && surface.alternate != active_planet_alternate)) continue;
        if (authored) {
            const auto& player = models[*model->second].authored_idle_probe;
            if (!player) return std::nullopt;
            auto pose = player->sample({static_cast<float>(animation_time), animation::PlaybackMode::loop, 0.0F});
            if (!pose || surface.bone < 0 || static_cast<std::size_t>(surface.bone) >= pose.value().bones.size())
                return std::nullopt;
            const auto matrix = animation::Player::multiply(
                animation::Player::asset_to_render_transform(pose.value().bones[surface.bone].skin_asset), surface.spin_reference);
            return std::array<float, 12>{matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6],
                matrix[8], matrix[9], matrix[10], matrix[12], matrix[13], matrix[14]};
        }
        for (const auto& instance : scenes[0].instances) {
            if (instance.asset_id != surface.asset || instance_planets.at(instance.entity_id) != options.zoom_probe_planet) continue;
            const auto evidence = renderer->bone_evidence(instance.entity_id, surface.bone);
            if (!evidence) return std::nullopt;
            // Compare actual rendered spin relative to each authored first
            // pose, preserving compensated mesh-local bases (e.g. Ryloth).
            animation::Matrix skin = animation::Player::identity_matrix();
            for (std::size_t column = 0; column < 4; ++column)
                for (std::size_t row = 0; row < 3; ++row)
                    skin[column * 4 + row] = (*evidence)[column * 3 + row];
            const auto matrix = animation::Player::multiply(skin, surface.spin_reference);
            return std::array<float, 12>{matrix[0], matrix[1], matrix[2], matrix[4], matrix[5], matrix[6],
                matrix[8], matrix[9], matrix[10], matrix[12], matrix[13], matrix[14]};
        }
    }
    return std::nullopt;
}

void GalacticMode::State::submit() {
    // WGUI-60/62: the camera switches its target between ALT0 and ALT1.
    // WGUI-05: an undiscovered planet shows its ALT2 surfaces (the authored grid
    // shell) whatever the camera does. Untagged surfaces always draw.
    active_planet_alternate = seat_flight().alternate_model() ? 1 : 0;
    for (std::size_t index = 0; index < scenes.size(); ++index) {
        GodotRenderer& target = index == 0 ? *renderer : *scenes[index].renderer;
        // WGP-04 / WGUI-60/62: alternates share the planet's animation clock.
        // Keep their live skeletons through zoom; visibility changes must not
        // introduce a newly created skeleton's rest pose (#2205).
        target.submit(std::make_shared<const sim::RenderSnapshot>(galaxy->tick, scenes[index].instances));
        scenes[index].submitted_instances = 0;
        for (const auto& instance : scenes[index].instances) {
            const auto object = instance_planets.at(instance.entity_id);
            const bool planet = galaxy_planet_ids.contains(object);
            galactic_view::PlanetShellInput art;
            art.galaxy_planet = planet;
            if (planet) {
                art.scene_art = std::find(scene_objects.begin(), scene_objects.end(), object) != scene_objects.end();
                art.discovered = art.scene_art || planet_discovered(object);
                art.has_shell = shell_planets.contains(object);
                art.world_layer = index == 0;
                art.camera_target = object == seat_flight().planet();
                art.camera_alternate = active_planet_alternate;
            }
            const int desired = galactic_view::planet_alternate(art);
            const int authored = asset_alternates.at(instance.asset_id);
            const bool visible = galactic_view::surface_drawn(authored, desired);
            target.set_instance_visible(instance.entity_id, visible);
            if (visible) ++scenes[index].submitted_instances;
        }
    }
}

// The #3270/#3272 GPU case's hook: the window resizes once during the campaign loading screen, so a
// capture run neither pins its viewport nor expects the capture identity's size.
[[nodiscard]] static bool resize_hook_requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index)
        if (arguments[index] == String("--eawr-resize-in-loading")) return true;
    return false;
}

bool GalacticMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        const auto argument = utf8(arguments[index]);
        if (argument == "--eawr-galactic" || argument == "--eawr-galactic-restore") return true;
    }
    return false;
}

GalacticMode::Options GalacticMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        const std::string argument = utf8(arguments[index]);
        const std::string value = utf8(arguments[index + 1]);
        bool consumed = true;
        if (argument == "--eawr-galactic") options.campaign = value;
        else if (argument == "--eawr-game-root") options.game_root = ViewerPath{value}.native();
        else if (argument == "--eawr-mod-root") options.mod_root = ViewerPath{value}.native();
        else if (argument == "--eawr-report") options.report_path = ViewerPath{value}.native();
        else if (argument == "--eawr-capture") options.capture_path = ViewerPath{value}.native();
        else if (argument == "--eawr-galactic-focus" || argument == "--eawr-galactic-flight-ms"
            || argument == "--eawr-galactic-zoom-probe") {
            std::uint32_t parsed_value{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsed_value);
            if (parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size()) {
                if (argument == "--eawr-galactic-focus") options.focus_planet = parsed_value;
                else if (argument == "--eawr-galactic-zoom-probe") options.zoom_probe_planet = parsed_value;
                else options.flight_milliseconds = parsed_value;
            } else options.camera_options_invalid = true;
        }
        else if (argument == "--eawr-galactic-seed") {
            std::uint64_t seed{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seed);
            options.proof_options_invalid |= parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size();
            options.capture_seed = seed;
        }
        else if (argument == "--eawr-galactic-ai") {
            galactic_view::NativeAiFlag flag{options.native_ai, options.native_ai_invalid};
            galactic_view::apply_native_ai_flag(flag, value);
            options.native_ai = flag.enabled;
            options.native_ai_invalid = flag.invalid;
        }
        else if (argument == "--eawr-galactic-reveal") {
            options.galactic_reveal = value == "on";
            options.galactic_reveal_invalid |= value != "on" && value != "off";
        }
        else if (argument == "--eawr-galactic-all-ai") {
            options.all_ai_capture = value == "yes";
            options.proof_options_invalid |= value != "yes";
        }
        else if (argument == "--eawr-galactic-space-proof") options.space_cycle_proof = value;
        else if (argument == "--eawr-galactic-restore") options.restore_save = Options::RestoreSave{{}, value};
        else if (argument == "--eawr-galactic-save-dir") options.save_directory = ViewerPath{value}.native();
        else if (argument == "--eawr-galactic-battle") {
            options.synthetic_battle = value == "synthetic";
            options.battle_invalid = options.battle_invalid || !options.synthetic_battle;
        } else if (argument == "--eawr-galactic-battle-frames") {
            std::uint64_t frames{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), frames);
            options.battle_invalid = options.battle_invalid || parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size() || frames == 0 || frames > 3600;
            options.battle_frames = frames;
        } else if (argument == "--eawr-galactic-battle-cycles") {
            std::uint32_t cycles{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), cycles);
            options.battle_invalid = options.battle_invalid || parsed.ec != std::errc{}
                || parsed.ptr != value.data() + value.size() || cycles == 0 || cycles > 8;
            options.battle_cycles = cycles;
        }
        else if (argument == "--eawr-galactic-paused") options.paused = value == "yes";
        else if (argument == "--eawr-galactic-coop-preview") options.coop_preview = value == "yes";
        else if (argument == "--eawr-galactic-hud-fixture") options.hud_fixture = value;
        else if (argument == "--eawr-galactic-queue") options.queue_type = value;
        else if (argument == "--eawr-galactic-queue-planet") {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.queue_planet);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || options.queue_planet == 0)
                options.camera_options_invalid = true;
        }
        else if (argument == "--eawr-galactic-red-green") options.red_green_routes = value == "yes";
        else if (argument == "--eawr-galactic-ticks") {
            const std::string& text = value;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.simulation_ticks);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                || options.simulation_ticks == 0) options.camera_options_invalid = true;
        }
        else if (argument == "--eawr-galactic-seat") {
            std::uint32_t seat{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seat);
            options.seat_invalid = parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || seat == 0;
            options.seat = options.seat_invalid ? 0 : seat;
        } else consumed = false;
        if (consumed) ++index;
    }
    options.proof_options_invalid |= options.capture_seed.has_value() && !options.all_ai_capture;
    if (!options.all_ai_capture && options.simulation_ticks > 3600) options.camera_options_invalid = true;
    return options;
}

GalacticMode::GalacticMode(Options options) : state_(std::make_unique<State>(std::move(options))) {}
GalacticMode::~GalacticMode() = default;
GalacticMode::GalacticMode(GalacticMode&&) noexcept = default;
GalacticMode& GalacticMode::operator=(GalacticMode&&) noexcept = default;

bool GalacticMode::ready(Node3D& host) {
    State& state = *state_;
    state.host = &host;
    state.interactive = state.options.capture_path.empty();
    const auto fail = [&]() {
        state.status = "failed";
        state.completed = true;
        state.overlay.release();
        static_cast<void>(state.write_report());
        UtilityFunctions::printerr(String::utf8(("EAWR galactic mode: " + state.failure).c_str()));
        return false;
    };
    // Both ordinary galaxies and synthetic handoffs start a galaxy session.
    // Let start_game submit the requested pause before displaying its publication;
    // pre-setting paused would make the toggle guard suppress that command.
    state.paused = false;
    if (!state.resolve_restore_save() || !state.mount() || !state.build_view() || !state.load_camera_constants()) return fail();
    if (state.restore_campaign) {
        // The save's seat must still command the faction it was saved by (mods may rename either).
        const auto& expected = state.restore_campaign->faction;
        const auto& local = state.galaxy->local.faction;
        if (!std::equal(local.begin(), local.end(), expected.begin(), expected.end(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); })) {
            state.failure = "EAWR-SAVE-0105: the saved seat commands " + local + " in the loaded data, not " + expected;
            return fail();
        }
    }

    const Vector2 viewport = host.get_viewport()->get_visible_rect().size;
    if (viewport.x >= 1.0F && viewport.y >= 1.0F) {
        state.camera.width = static_cast<std::uint32_t>(viewport.x);
        state.camera.height = static_cast<std::uint32_t>(viewport.y);
    }
    // A capture reads the viewport back at the requested size; an interactive
    // run follows its window.
    // --eawr-resize-in-loading (the #3270/#3272 GPU case) keeps the window free so the resize reaches the battle.
    const bool follows_window = resize_hook_requested();
    if (!state.interactive && !follows_window)
        pin_capture_viewport(*host.get_window(), state.camera.width, state.camera.height);
    // WGP-41/42: authored overview pitch/pullback, positioned at the local
    // faction's first Home_Location object, revealed by campaign observer startup.
    if (state.content && state.options.campaign != galactic_view::synthetic_campaign) {
        if (const auto* home = galactic_view::initial_home(*state.content, state.galaxy->local.player)) {
            const auto drawable = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
                [&](const auto& planet) { return planet.id == home->id; });
            if (drawable != state.galaxy->planets.end()) state.seat_flight().position(drawable->position);
            state.selected_planet = home->id;
        }
    }
    if (state.options.all_ai_capture || (state.options.campaign == galactic_view::synthetic_campaign
        && state.options.hud_fixture.starts_with("corruption-spread"))) {
        // Capture the playable galaxy rather than only the authored starting home.
        auto camera_metadata = state.metadata;
        std::erase_if(camera_metadata.planets, [&](const auto& planet) {
            return std::find(state.scene_objects.begin(), state.scene_objects.end(), planet.id) != state.scene_objects.end();
        });
        camera_metadata.fleets.clear();
        const auto camera_view = galactic_view::build_view(camera_metadata, state.galaxy->local.seat);
        if (!camera_view.view) { state.failure = camera_view.failure; return fail(); }
        const auto fitted = galactic_view::overview_camera(*camera_view.view,
            static_cast<float>(state.camera.width) / static_cast<float>(state.camera.height),
            galactic_view::galactic_vertical_fov(state.camera.width, state.camera.height));
        state.camera_constants.initial_pullback = fitted.distance;
        state.camera_constants.initial_pitch = -fitted.pitch_degrees;
        state.seat_flight() = galactic_view::PlanetCamera(state.camera_constants);
        state.seat_flight().position(fitted.target);
    }
    state.overview = state.seat_flight().pose();
    // ADR-019: independent worlds let the galaxy retain all its scene resources
    // while tactical cameras, lights and environment own the main viewport.
    state.galaxy_container = memnew(SubViewportContainer);
    host.add_child(state.galaxy_container);
    state.galaxy_container->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    state.galaxy_container->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    state.galaxy_container->set_stretch(true);
    state.galaxy_viewport = memnew(SubViewport);
    state.galaxy_container->add_child(state.galaxy_viewport);
    state.galaxy_container->set_size(Vector2(static_cast<float>(state.camera.width), static_cast<float>(state.camera.height)));
    state.galaxy_viewport->set_world_3d(Ref<World3D>(memnew(World3D)));
    state.galaxy_viewport->set_update_mode(SubViewport::UPDATE_ALWAYS);
    state.galaxy_host = memnew(Node3D);
    state.galaxy_viewport->add_child(state.galaxy_host);
    if (!state.build_scenes() || !state.upload_models() || !state.build_overlay() || !state.build_routes() || !state.build_hud()) return fail();
    if (state.options.focus_planet != 0) {
        const auto selected = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
            [&](const auto& planet) { return planet.id == state.options.focus_planet; });
        if (selected == state.galaxy->planets.end() || !state.planet_models.at(selected->id)) {
            state.failure = "the requested focus planet has no drawn model";
            return fail();
        }
        state.selected_planet = selected->id;
        static_cast<void>(state.seat_flight().zoom_in(*selected, true));
        if (!state.interactive) {
            state.seat_flight().service(0);
            state.seat_flight().service(state.options.flight_milliseconds);
        }
    }
    state.apply_camera();
    if (!state.update_routes()) return fail();
    state.map_input.selected = state.selected_planet;
    if (state.content) {
        if (const auto* home = galactic_view::initial_home(*state.content, state.galaxy->local.player))
            state.map_input.home = home->id;
    }
    state.submit();
    if (!state.advance_animation(0.0)) return fail();
    if (state.options.synthetic_battle) {
        if (!state.prepare_handoff()) return fail();
    } else if (!state.start_game({})) return fail();
    const vfs::Vfs* audio_filesystem = state.battle_content ? state.battle_content->filesystem.get()
        : (state.filesystem ? &*state.filesystem : nullptr);
    if (audio_filesystem && state.audio_catalog) {
            bool muted = !state.interactive;
            const auto args = OS::get_singleton()->get_cmdline_user_args();
            for (std::int64_t index = 0; index + 1 < args.size(); ++index)
                if (utf8(args[index]) == "--eawr-audio") muted = muted || utf8(args[index + 1]) == "off";
            state.audio = std::make_unique<GalacticAudio>(*state.galaxy_host, *audio_filesystem,
                *state.audio_catalog, state.galaxy->local.faction, muted);
            state.audio->ambient(); // WGP-23/24: local faction, strategic mode entered.
            if (!state.queued_underway.empty()) state.audio->build(audio::GalacticBuildCue::underway, state.queued_underway, true);
    }
    // Real and restored child scenes borrow this retained catalog.
    state.battle_choice_sink = [owner = &state](const std::uint64_t conflict, const galactic_view::BattleChoice choice) {
        if (!owner->game || !owner->galaxy) return false;
        if (choice == galactic_view::BattleChoice::continue_galaxy) {
            if (owner->battle_dialog && owner->battle_dialog->view().phase == galactic_view::BattleDialogPhase::summary
                && owner->battle_dialog->view().conflict == conflict)
                return galactic_view::acknowledge_battle_summary(owner->acknowledged_battle_summary, owner->battle_dialog_frame(), conflict);
            return owner->acknowledged_battle_summary.contains(conflict);
        }
        if (choice == galactic_view::BattleChoice::invade) {
            const auto frame = owner->game->frame();
            if (!frame.invasion_offer || frame.invasion_offer->conflict != conflict
                || frame.invasion_offer->observer != owner->galaxy->local.player
                || !frame.invasion_offer->visible_admitted || !frame.invasion_offer->accept_permitted) return false;
            if (!galactic_view::acknowledge_battle_summary(owner->acknowledged_battle_summary, frame, conflict)) return false;
            owner->game->submit(platform::LiveGameOrder{sim::SeatId{owner->galaxy->local.seat},
                sim::galactic::PauseGalaxy{}, {}, conflict});
            ++owner->submitted_orders;
            return true;
        }
        if (choice == galactic_view::BattleChoice::retreat) {
            const auto order = platform::auto_resolve_withdraw_order(owner->game->frame(),
                sim::SeatId{owner->galaxy->local.seat}, conflict);
            if (order) { owner->game->submit(*order); ++owner->submitted_orders; }
            return order.has_value();
        }
        if (choice != galactic_view::BattleChoice::fight && choice != galactic_view::BattleChoice::auto_resolve) return false;
        const auto order = platform::battle_choice_order(owner->game->frame(), sim::SeatId{owner->galaxy->local.seat},
            conflict, choice == galactic_view::BattleChoice::fight);
        if (order) { owner->game->submit(*order); ++owner->submitted_orders; }
        return order.has_value();
    };
    state.save_controls = std::make_unique<GalacticSaveControls>(state);
    if (state.options.restore_save && !state.save_controls->restore_at_start(state.options.restore_save->name)) return fail();
    if (const std::string perf_error = parse_perf_overlay_argument(OS::get_singleton()->get_cmdline_user_args(), state.perf_requested);
        !perf_error.empty()) {
        state.failure = perf_error;
        return fail();
    }
    if (state.perf_requested) state.set_perf_overlay(true);
    if (state.interactive) {
        UtilityFunctions::print("EAWR galactic mode: pan with WASD or the arrows and right-drag, select with left-click, "
                                "right-click a planet with a selected fleet to move or invade, double-click toggles focus, wheel controls focus and the advanced-map state, Home restores overview; close the window to exit "
                                "(project bindings)");
    }
    return true;
}

std::optional<int> GalacticMode::process(const double delta) {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    ++state.frames;
    if (state.save_controls) state.save_controls->service();
    if (!state.options.space_cycle_proof.empty() && !state.service_space_cycle_proof()) {
        state.completed = true; state.status = "failed";
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.game && state.battle_dialog && state.galaxy && !state.options.hud_fixture.starts_with("battle-")) {
        const auto frame = state.battle_dialog_frame();
        if (!state.battle_pending_publication.reuse(frame, sim::SeatId{state.galaxy->local.seat},
            state.galaxy->local.player, state.battle_metadata_generation)
            && !state.battle_summary_publication.reuse(frame, sim::SeatId{state.galaxy->local.seat},
            state.galaxy->local.player, state.acknowledged_battle_summary, state.battle_metadata_generation,
            state.presented_conflict.get())) {
            galactic_view::LiveBattleDialogNames names;
            names.bounty = galactic_view::live_campaign_bounty_namer(frame, state.battle_text);
            names.planet = [&state](const auto id) {
                const auto found = std::find_if(state.metadata.planets.begin(), state.metadata.planets.end(),
                    [&](const auto& row) { return row.id == id; });
                if (found == state.metadata.planets.end()) return std::string{};
                if (state.audio_catalog) {
                    auto planet = state.audio_catalog->resolve(found->name, data::Category::game_object);
                    if (planet) if (const auto* key = planet.value().value("Text_ID"))
                        return state.battle_text(key->value.raw_text);
                }
                return found->name;
            };
            names.side = [&state](const auto owner) {
                galactic_view::BattleDialogSide side;
                if (state.content) if (const auto* affiliation = galactic_view::affiliation_for_owner(*state.content, owner)) {
                    side.name = affiliation->faction; side.icon = affiliation->icon;
                    if (state.audio_catalog) {
                        auto faction = state.audio_catalog->resolve(affiliation->faction, data::Category::faction);
                        if (faction) if (const auto* key = faction.value().value("Text_ID"))
                            side.name = state.battle_text(key->value.raw_text);
                    }
                    side.colour = {affiliation->primary.r, affiliation->primary.g, affiliation->primary.b};
                }
                return side;
            };
            names.type = [&state](const std::string_view type) {
                const auto at = state.battle_result_types.find(std::string(type));
                return at == state.battle_result_types.end() ? galactic_view::CampaignResultType{std::string(type), false, false, 0, 0}
                    : at->second;
            };
            names.force = [&state, info = names.type](const auto id) {
                const auto at = state.production_names.find(id);
                return at == state.production_names.end() ? std::string{} : info(at->second).name;
            };
            names.force_icon = [&state](const auto id) {
                const auto at = state.production_names.find(id);
                if (at == state.production_names.end()) return std::string{};
                const auto art = state.production_art.find(at->second);
                return art == state.production_art.end() ? std::string{} : art->second.icon;
            };
            names.force_health = [&state](const auto id) -> std::optional<float> {
                const auto at = state.production_names.find(id);
                if (at == state.production_names.end()) return std::nullopt;
                const auto art = state.production_art.find(at->second);
                return art == state.production_art.end() ? std::nullopt : art->second.max_health;
            };
            names.original_type = [&state](const auto id) {
                const auto at = state.production_names.find(id);
                return at == state.production_names.end() ? std::string{} : at->second;
            };
            names.tactical_type = [&state](const auto id) {
                const auto at = state.battle_tactical_types.find(id);
                return at == state.battle_tactical_types.end() ? std::string{} : at->second;
            };
            names.space_picture = [&state](const auto player, const bool won) {
                const auto at = state.battle_space_pictures.find(player);
                return at == state.battle_space_pictures.end() ? std::string{} : at->second[won ? 0 : 1];
            };
            auto dialog = galactic_view::live_battle_dialog_view(frame,
                sim::SeatId{state.galaxy->local.seat}, state.galaxy->local.player, state.acknowledged_battle_summary,
                names, frame.tactical_available, state.presented_conflict.get());
            if (dialog.phase == galactic_view::BattleDialogPhase::choice && !frame.tactical_refusal.empty()
                && dialog.refusal.empty()) dialog.refusal = frame.tactical_refusal;
            state.battle_dialog->present(std::move(dialog));
        }
    }
    state.service_battle_dialog(delta);
    state.service_campaign_score();
    state.sync_perf_overlay();
    if (state.leave_requested) {
        state.completed = true; state.status = "menu-exit";
        static_cast<void>(state.write_report());
        return std::nullopt;
    }
    if (state.score_destination && *state.score_destination != platform::CampaignScoreDestination::none) {
        // The exit/continuation request of #2837's score callback is only published: the
        // mode finishes (inert from the next frame, report written) and keeps running, and the
        // host decides what the destination means (`score_destination()`).
        state.completed = true; state.status = "score-exit";
        static_cast<void>(state.write_report());
        return std::nullopt;
    }
    if (state.audio) state.audio->service(delta, state.camera, state.paused);
    if (state.game && state.audio) {
        const auto events = state.game->galactic_events_after(state.audio_event_cursor);
        // WGP-187: notifications can be newer than the frame already drawn.
        // Read after the event batch so merged fleet membership is current.
        const auto audio_snapshot = state.game->frame().latest;
        if (events.incomplete) ++state.audio_event_gaps;
        if (audio_snapshot && audio_snapshot->production())
            for (const auto& planet : audio_snapshot->production()->planets)
                for (const auto& queue : planet.queues)
                    for (const auto& entry : queue)
                        if (entry.builder == state.galaxy->local.player) state.local_builds.insert(entry.build_id);
        for (const auto& published : events.events) {
            if (published.feedback && published.feedback->player == state.galaxy->local.player
                && published.feedback->kind == sim::galactic::GalacticFeedbackNotice::Kind::enemy_approaching)
                state.enqueue_approach(published.feedback->planet);
            state.audio->consume(published, state.galaxy->local.player);
            using Kind = sim::galactic::GalacticEventKind;
            const auto& event = published.event;
            if (event.kind == Kind::production_started && event.producer == sim::SeatId{state.galaxy->local.seat})
                state.local_builds.insert(event.sequence);
            // WGP-181: an accepted local move order; a rejected order or a paused galaxy is silent.
            if (event.kind == Kind::movement_begin && event.producer == sim::SeatId{state.galaxy->local.seat}
                && !state.paused && audio_snapshot) {
                const auto fleets = audio_snapshot->fleets();
                const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == event.subject; });
                if (fleet != fleets.end()) {
                    std::vector<std::string> types;
                    for (const auto& member : fleet->members)
                        if (const auto name = state.production_names.find(member.type); name != state.production_names.end())
                            types.push_back(name->second);
                    state.audio->fleet_move(types);
                }
            }
            if (event.kind != Kind::production_completed && event.kind != Kind::production_cancelled) continue;
            // WGEP-96/WGP-63: completion uses the recorded builder; an explicit
            // cancellation also knows its supplying player.
            const bool tracked_builder = state.local_builds.erase(event.sequence) > 0;
            // WGEP-96: an explicit cancellation uses its supplying player.
            // An entry can disappear before the first observer queue sample.
            const bool local_cancel = event.kind == Kind::production_cancelled
                && std::find(state.galaxy->local.faction_seats.begin(), state.galaxy->local.faction_seats.end(),
                    event.producer.value) != state.galaxy->local.faction_seats.end();
            const bool local = tracked_builder || local_cancel;
            const auto name = state.production_names.find(static_cast<sim::galactic::TypeId>(event.value));
            if (name != state.production_names.end())
                state.audio->build(event.kind == Kind::production_completed ? audio::GalacticBuildCue::completed
                    : audio::GalacticBuildCue::stopped, name->second, local);
        }
        state.audio_event_cursor = events.through;
    }
    state.service_approach(delta);
    if (state.game && state.options.synthetic_battle && state.process_handoff(delta)) return std::nullopt;
    if (state.game && !state.options.synthetic_battle && state.process_battle_transition(delta)) return std::nullopt;
    if (!state.options.space_cycle_proof.empty() && state.failure.empty()
        && (state.cycles.empty() || !state.proof_summary_acknowledged
            || (state.battle_dialog && state.battle_dialog->visible()))) return std::nullopt;
    // The diagnostic clock traverses long stock idle clips deterministically.
    // Live and paused presentation keep their ordinary clock semantics.
    const bool refreshed = state.failure.empty() && state.refresh_game();
    const bool station_changed = refreshed && state.service_station_rotation(delta);
    if (!refreshed || !state.failure.empty()
        || !state.advance_animation(state.options.zoom_probe_planet ? 0.5 : delta)) {
        state.completed = true;
        state.status = "failed";
        static_cast<void>(state.write_report());
        return 2;
    }
    if (state.options.zoom_probe_planet != 0) {
        const auto planet = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
            [&](const auto& value) { return value.id == state.options.zoom_probe_planet; });
        if (planet == state.galaxy->planets.end() || !state.planet_models.at(planet->id)) {
            state.failure = "zoom probe requires a drawn planet";
            close();
            return 2;
        }
        const auto before = state.zoom_pose();
        const auto authored_before = state.zoom_pose(true);
        const auto previous = state.active_planet_alternate;
        if (state.frames == 30 || state.frames == 150) static_cast<void>(state.seat_flight().zoom_in(*planet, true));
        if (state.frames == 90) static_cast<void>(state.seat_flight().zoom_out());
        state.seat_flight().service(state.frames * 20);
        state.apply_camera();
        state.submit();
        const auto after = state.zoom_pose();
        const auto authored_after = state.zoom_pose(true);
        if (previous != state.active_planet_alternate && before && after)
            state.zoom_samples.push_back({state.frames, state.animation_time, state.active_planet_alternate, *before, *after,
                authored_before.value_or(*before), authored_after.value_or(*after)});
        if (state.frames < 210) return std::nullopt;
    }
    if (state.interactive) {
        if (state.map_input.delay) {
            const auto planets = state.selection_planets();
            state.apply_selection(state.map_input.service_deferred(planets, state.seat_flight().state()));
        }
        state.seat_flight().service(Time::get_singleton()->get_ticks_msec());
        float right = 0.0F;
        float forward = 0.0F;
        for (const std::int64_t key : state.held) {
            if (key == KEY_A || key == KEY_LEFT) right -= 1.0F;
            if (key == KEY_D || key == KEY_RIGHT) right += 1.0F;
            if (key == KEY_W || key == KEY_UP) forward += 1.0F;
            if (key == KEY_S || key == KEY_DOWN) forward -= 1.0F;
        }
        if (right != 0.0F || forward != 0.0F) {
            // GMC_InitialPullbackDistance supplies the scale for the project keyboard-pan binding.
            const float step = state.camera_constants.initial_pullback * keyboard_pan_fraction
                * static_cast<float>(std::clamp(delta, 0.0, keyboard_pan_max_delta));
            state.seat_flight().pan(*state.galaxy, right * step, -forward * step);
        }
        state.apply_camera();
        state.submit(); // WGUI-60/62: camera state changes the target's authored mesh set.
        if (!state.update_routes()) {
            state.status = "failed";
            state.completed = true;
            static_cast<void>(state.write_report());
            return 2;
        }
        return std::nullopt;
    }
    if (station_changed && state.options.zoom_probe_planet == 0) state.submit();
    if (!state.update_routes()) {
        state.status = "failed";
        state.completed = true;
        static_cast<void>(state.write_report());
        return 2;
    }
    // Presented frames before the capture, so shaders and textures are live.
    if (state.game && !state.options.synthetic_battle && !state.paused) {
        if (state.options.all_ai_capture) {
            // One completed publication at a time: event owners and per-minute state cannot be skipped.
            if (!state.displayed_snapshot || state.capture_metrics.observed_session_tick < state.proof_requested_tick)
                return std::nullopt;
            const auto target = std::min(state.proof_capture_frame, state.options.simulation_ticks);
            if (state.displayed_snapshot->clock().galactic_frame < target) {
                state.proof_requested_tick = state.capture_metrics.observed_session_tick + 1;
                state.game->advance_to(state.proof_requested_tick);
                return std::nullopt;
            }
        } else {
            state.game->advance_to(state.options.simulation_ticks);
            if (state.game->completed_tick() < state.options.simulation_ticks) return std::nullopt;
        }
    }
    // The capture focus is an explicit selection intent. A named campaign's
    // initial publication may precede discovery; retry through the ordinary
    // discovery gate once the requested simulation publication reaches the HUD.
    if (state.options.focus_planet != 0 && state.map_input.selected != state.options.focus_planet) {
        const auto planets = state.selection_planets();
        state.apply_selection(state.map_input.select(planets, state.options.focus_planet, state.seat_flight().state()));
    }
    if (state.warmup > 0) {
        --state.warmup;
        return std::nullopt;
    }
    const bool proof_more = state.options.all_ai_capture
        && state.displayed_snapshot->clock().galactic_frame < state.options.simulation_ticks;
    state.completed = !proof_more;
    auto captured = state.capture_renderer->capture(state.camera);
    if (!captured) {
        state.failure = core::format_diagnostic(captured.error());
    } else if (const std::string problem = capture_size_problem(captured.value(), state.camera);
               !problem.empty() && !resize_hook_requested()) {
        state.failure = problem;
    } else {
        const std::vector<std::byte>& png = captured.value().png_bytes;
        state.capture_sha256 = hash_bytes(png);
        state.capture_width = captured.value().width;
        state.capture_height = captured.value().height;
        std::error_code error;
        if (!state.options.capture_path.parent_path().empty()) {
            std::filesystem::create_directories(state.options.capture_path.parent_path(), error);
        }
        auto path = state.options.capture_path;
        if (state.options.all_ai_capture) {
            const auto frame = state.displayed_snapshot->clock().galactic_frame;
            path = path.parent_path() / (path.stem().string() + "-frame-" + std::to_string(frame) + ".png");
            state.ai_capture_frames.push_back(frame);
        }
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!output) state.failure = "the capture PNG could not be written";
    }
    if (proof_more && state.failure.empty()) {
        state.proof_capture_frame += 1800;
        state.warmup = 2;
        return std::nullopt;
    }
    if (state.ai_activity && !state.options.all_ai_capture && state.displayed_snapshot)
        state.capture_metrics.sample(*state.displayed_snapshot); // end checkpoint
    if (state.options.all_ai_capture) {
        state.capture_metrics.sample(*state.displayed_snapshot);
        state.game->stop();
        const auto hashes = state.game->tick_hashes();
        if (!hashes.empty()) state.capture_metrics.end_state_hash = hashes.back();
    }
    state.status = state.failure.empty() ? "captured" : "failed";
    const bool written = state.write_report();
    return state.failure.empty() && written ? 0 : 2;
}

void GalacticMode::input(const Ref<InputEvent>& event) {
    State& state = *state_;
    // #4031: the performance overlay's key (F3), in any galactic run; nothing else sees it.
    if (const auto* pressed = Object::cast_to<InputEventKey>(event.ptr());
        pressed != nullptr && pressed->is_pressed() && !pressed->is_echo() && !pressed->is_shift_pressed()
        && !pressed->is_ctrl_pressed() && !pressed->is_alt_pressed() && !pressed->is_meta_pressed()
        && (pressed->get_physical_keycode() != KEY_NONE ? pressed->get_physical_keycode() : pressed->get_keycode())
            == KEY_F3) {
        state.set_perf_overlay(state.perf == nullptr || !state.perf->shown());
        return;
    }
    if (state.campaign_score && state.campaign_score->visible()) {
        // WGP-37/111: the score is modal and topmost; Escape acts like its button.
        const auto* key = Object::cast_to<InputEventKey>(event.ptr());
        if (key && key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE)
            state.campaign_score->activate_quit(true);
        cancel_input();
        return;
    }
    if (state.save_controls && state.save_controls->input(event)) return;
    if (state.retreat_dialog_open) { cancel_input(); return; }
    if (state.tactical) { state.tactical->input(event); return; }
    if (state.loading) return;
    if (state.battle_dialog && state.battle_dialog->visible()) {
        const auto* mouse = Object::cast_to<InputEventMouse>(event.ptr());
        if (state.battle_dialog->view().phase != galactic_view::BattleDialogPhase::choice) { cancel_input(); return; }
        // WGUI-171: blocking the prompt's mouse surface must not release a held camera key.
        if (mouse && state.battle_dialog->blocks_world(mouse->get_position())) { pointer_left(); return; }
    }
    if (state.hero_menu.blocker && state.hero_menu.blocker->is_visible_in_tree()) { cancel_input(); return; }
    if (state.weapon_menu.blocker && state.weapon_menu.blocker->is_visible_in_tree()) { cancel_input(); return; }
    if (state.corruption_menu.blocker && state.corruption_menu.blocker->is_visible_in_tree()) { cancel_input(); return; }
    if (!state.interactive || state.completed || event.is_null() || !state.galaxy) return;
    if (!state.map_input.gates.world_allowed()) return;
    if (state.map_input.gates.tutorial_locked) { cancel_input(); return; }
    auto& flight = state.seat_flight();
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        const std::int64_t code = key->get_keycode();
        if (key->is_pressed()) {
            if (code == KEY_HOME && key->is_ctrl_pressed()) {
                const auto planets = state.input_planets();
                galactic_view::ObjectId leader_planet{};
                if (!state.map_input.home && state.content && state.displayed_snapshot) {
                    const auto type = galactic_view::home_leader_type(*state.content, state.galaxy->local.player, state.production_names);
                    leader_planet = galactic_view::first_hero_planet(*state.displayed_snapshot, type);
                }
                state.apply_selection(state.map_input.go_home(planets, leader_planet, flight.state()));
            } else if (code == KEY_HOME) {
                flight = galactic_view::PlanetCamera(state.camera_constants);
                state.apply_camera(); // GHUDL-11: reset also dismisses the organization shell.
            }
            else if (code == KEY_PAGEUP && !key->is_echo()) navigate(galactic_view::Navigation::previous);
            else if (code == KEY_PAGEDOWN && !key->is_echo()) navigate(galactic_view::Navigation::next);
            else if (code == KEY_SPACE && !key->is_echo()
                && (!state.command_bar || state.clock_controls_enabled)) state.request_pause(!state.paused);
            else state.held.insert(code);
        } else {
            state.held.erase(code);
        }
        return;
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        const auto index = button->get_button_index();
        if (index == MOUSE_BUTTON_RIGHT && button->is_pressed() && state.selected_fleet
            && state.displayed_snapshot) {
            const auto position = button->get_position();
            if (const auto* picked = state.picked_planet(position.x, position.y)) {
                const auto planets = state.input_planets();
                if (const auto intent = galactic_view::fleet_destination_intent(*state.published_snapshot,
                        state.galaxy->local.player, state.selected_fleet, picked->id, planets)) {
                    // #3072: a selected whole card moves every fleet of its slot as one stack.
                    std::vector<galactic_view::ObjectId> stack;
                    if (!state.selected_fleet_member && !state.tactical && !state.loading && state.game)
                        for (const auto& fleet : state.published_snapshot->fleets())
                            if (fleet.id == state.selected_fleet) {
                                const std::array slots{fleet.orbit_slot};
                                stack = galactic_view::stack_fleets(*state.published_snapshot, state.galaxy->local.player, fleet.orbit, slots);
                            }
                    if (stack.size() > 1 ? state.submit_fleet_stack(*intent, stack) : widget_drop(*intent)) {
                        state.scrolling = state.map_input.held_scroll = false;
                        state.pointer_warp.reset();
                        return;
                    }
                }
            }
        }
        if (index == MOUSE_BUTTON_RIGHT) state.scrolling = button->is_pressed()
            && flight.state() == galactic_view::CameraState::overview;
        if (index == MOUSE_BUTTON_RIGHT) state.pointer_warp.reset();
        if (index == MOUSE_BUTTON_MIDDLE) state.orbiting = button->is_pressed();
        state.map_input.held_scroll = state.scrolling;
        state.map_input.held_rotate = state.orbiting;
        if (!button->is_pressed()) {
            if (index == MOUSE_BUTTON_LEFT) static_cast<void>(state.map_input.release_widget([](auto) {}));
            return;
        }
        if (index != MOUSE_BUTTON_LEFT && index != MOUSE_BUTTON_WHEEL_UP && index != MOUSE_BUTTON_WHEEL_DOWN) return;
        const auto planets = index == MOUSE_BUTTON_WHEEL_DOWN ? std::vector<galactic_view::InputPlanet>{} : state.selection_planets();
        const auto position = button->get_position();
        const auto* picked = state.picked_planet(position.x, position.y);
        if (index == MOUSE_BUTTON_LEFT && picked) {
            state.apply_selection(state.map_input.world_select(planets, picked->id, flight.state(), button->is_double_click()));
        } else if (index == MOUSE_BUTTON_LEFT) {
            state.apply_selection(state.map_input.cancel_world(planets, flight.state()));
        }
        if (!state.map_input.wheel_zoom()) return; // WGUI-78: wheel consumed during drag.
        if (index == MOUSE_BUTTON_WHEEL_UP) {
            if (picked && flight.state() == galactic_view::CameraState::overview)
                state.apply_selection(state.map_input.world_select(planets, picked->id, flight.state()));
            const auto selected = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
                [&](const auto& planet) { return planet.id == state.selected_planet; });
            if (selected != state.galaxy->planets.end() && state.planet_models.at(selected->id)) {
                static_cast<void>(flight.zoom_in(*selected, true));
                state.apply_camera(); // GHUDL-11: advanced-map transitions need an immediate HUD refresh.
            }
        }
        if (index == MOUSE_BUTTON_WHEEL_DOWN && flight.state() == galactic_view::CameraState::focused) {
            static_cast<void>(flight.zoom_out()); // WGUI-65: explicit outward command requires focus.
            state.apply_camera();
        }
        return;
    }
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        if (!state.scrolling) return;
        const Vector2 position = motion->get_position();
        if (state.pointer_warp) {
            const auto expected = *state.pointer_warp;
            state.pointer_warp.reset();
            if (std::abs(position.x - expected[0]) < 0.5F && std::abs(position.y - expected[1]) < 0.5F) return;
        }
        const Vector2 relative = motion->get_relative();
        if (relative == Vector2()) return;
        if (flight.right_drag(*state.galaxy, relative.x / static_cast<float>(state.camera.width),
                relative.y / static_cast<float>(state.camera.height), true, motion->is_ctrl_pressed())) {
            // WGUI-78: retain the cursor's pre-move point; ignore the OS warp echo.
            const Vector2 old_position = position - relative;
            state.pointer_warp = std::array<float, 2>{old_position.x, old_position.y};
            state.host->get_viewport()->warp_mouse(old_position);
        }
    }
}

void GalacticMode::cancel_input() {
    if (state_->dragged_fleet && state_->host) state_->host->get_viewport()->gui_cancel_drag();
    state_->cancel_fleet_drag();
    if (state_->tactical) state_->tactical->focus(false);
    state_->held.clear();
    state_->orbiting = false;
    state_->scrolling = false;
    state_->pointer_warp.reset();
    state_->map_input.cancel_pointer();
    state_->ground_hud.pressed = 0; state_->ground_hud.double_release = false;
    if (state_->ground_hud.input) state_->ground_hud.input->queue_redraw();
}

void GalacticMode::focus(const bool focused) {
    if (!focused) cancel_input();
    else if (state_->tactical) state_->tactical->focus(true);
}

void GalacticMode::pointer_left() {
    if (state_->dragged_fleet && state_->host) state_->host->get_viewport()->gui_cancel_drag();
    state_->cancel_fleet_drag();
    if (state_->tactical) state_->tactical->pointer_left();
    state_->orbiting = false;
    state_->scrolling = false;
    state_->pointer_warp.reset();
    state_->map_input.cancel_pointer();
    state_->ground_hud.pressed = 0; state_->ground_hud.double_release = false;
    if (state_->ground_hud.input) state_->ground_hud.input->queue_redraw();
}

void GalacticMode::input_gates(galactic_view::InputGates gates) {
    state_->map_input.gates = gates;
    if (gates.tutorial_locked || !gates.world_allowed()) cancel_input();
}

bool GalacticMode::select_planet(const std::uint64_t planet, const bool delayed) {
    if (!state_->galaxy || state_->tactical || state_->loading) return false;
    const auto planets = state_->selection_planets();
    const auto effect = state_->map_input.select(planets, planet, state_->seat_flight().state(), delayed);
    state_->apply_selection(effect);
    return effect.accepted;
}

void GalacticMode::navigate(const galactic_view::Navigation direction) {
    if (!state_->galaxy || state_->tactical || state_->loading) return;
    const auto planets = state_->selection_planets();
    state_->apply_selection(state_->map_input.navigate(planets, state_->galaxy->local.player, direction,
        state_->seat_flight().state()));
}

void GalacticMode::select_hero(const sim::galactic::TypeId type, const bool inactive, const bool twice) {
    if (!type || inactive || state_->map_input.gates.selection_locked
        || !state_->galaxy || state_->tactical || state_->loading) return;
    const auto planets = state_->input_planets();
    const auto containing_planet = state_->displayed_snapshot
        ? galactic_view::first_hero_planet(*state_->displayed_snapshot, type) : 0;
    state_->apply_selection(state_->map_input.hero(planets, type != 0, inactive, containing_planet,
        state_->seat_flight().state(), twice));
}

bool GalacticMode::widget_drop(const galactic_view::DropIntent& intent) {
    if (!state_->galaxy || !state_->game || state_->tactical || state_->loading) return false;
    const auto planets = state_->input_planets();
    const auto command = state_->map_input.command(intent, planets);
    if (!command) return false;
    state_->game->submit(platform::LiveGameOrder{sim::SeatId{state_->galaxy->local.seat}, *command, {}});
    ++state_->submitted_orders;
    return true;
}

bool GalacticMode::choose_battle(const std::uint64_t conflict, const bool tactical) {
    if (!state_->galaxy || !state_->game || state_->tactical || state_->loading) return false;
    const auto order = platform::battle_choice_order(state_->game->frame(),
        sim::SeatId{state_->galaxy->local.seat}, conflict, tactical);
    if (!order) return false;
    state_->game->submit(*order);
    ++state_->submitted_orders;
    return true;
}

bool GalacticMode::begin_widget_drag(const galactic_view::InputWidget& widget, const bool control,
    const bool focused_fleet, const std::function<void()>& merge_home) {
    if (!state_->galaxy || state_->tactical || state_->loading) return false;
    return state_->map_input.begin_widget(widget, control, focused_fleet, merge_home);
}

bool GalacticMode::release_widget_drag(
    const std::function<std::optional<galactic_view::DropIntent>(std::uint64_t)>& resolve) {
    if (!state_->galaxy || !resolve) return false;
    return state_->map_input.release_widget([&](const auto id) {
        if (const auto intent = resolve(id)) static_cast<void>(widget_drop(*intent));
    });
}

bool GalacticMode::begin_ground_box(const galactic_view::PointerPoint point, const bool allied, const bool valid_index) {
    if (!state_->galaxy || state_->tactical || state_->loading) return false;
    return state_->map_input.begin_box(point, allied, valid_index);
}

void GalacticMode::move_ground_box(const galactic_view::PointerPoint point) { state_->map_input.move_box(point); }

bool GalacticMode::release_ground_box(const std::span<const galactic_view::InputWidget> widgets) {
    return state_->map_input.release_box(widgets);
}

bool GalacticMode::radar_click(const galactic_view::PointerPoint point, const float width, const float height) {
    return state_->request_radar_position(point, width, height);
}

bool GalacticMode::leave_requested() const { return state_->leave_requested; }

std::optional<platform::CampaignScoreDestination> GalacticMode::score_destination() const {
    return state_->score_destination;
}

std::string GalacticMode::failure() const { return state_->failure; }

// #3035: the stored identity prepares the campaign; prepare_load then validates the save itself.
bool GalacticMode::State::resolve_restore_save() {
    if (!options.restore_save) return true;
    const auto refuse = [&](std::string text) { failure = "EAWR-SAVE-0105: " + std::move(text); return false; };
    if (options.restore_save->name.empty()) return refuse("--eawr-galactic-restore needs a save name");
    if (options.synthetic_battle || !options.space_cycle_proof.empty() || !options.hud_fixture.empty()
        || !options.queue_type.empty() || options.paused) return refuse("a restored campaign takes no start fixtures");
    auto slot = platform::read_save_slot(ViewerPath::utf8(save_directory(options)), options.restore_save->name);
    if (!slot) { failure = core::format_diagnostic(slot.error()); return false; }
    if (slot.value().diagnostic) { failure = core::format_diagnostic(*slot.value().diagnostic); return false; }
    if (!slot.value().campaign) return refuse("save " + options.restore_save->name + " records no campaign");
    const auto& identity = *slot.value().campaign;
    // Retail's ordinary load list skips multiplayer saves (WGS-161); they resume through a lobby.
    if (identity.multiplayer) return refuse("save " + options.restore_save->name + " belongs to a multiplayer campaign");
    if (!options.campaign.empty() && options.campaign != identity.campaign)
        return refuse("save " + options.restore_save->name + " belongs to campaign " + identity.campaign);
    if (options.seat != 0 && options.seat != identity.seat) return refuse("save was made by another seat");
    options.campaign = identity.campaign; options.seat = identity.seat;
    difficulty = identity.difficulty;
    restore_campaign = identity;
    return true;
}

void GalacticMode::close() {
    State& state = *state_;
    // A child feed retains the campaign owner; release it before the root.
    // Window shutdown keeps the loading/conflict facts for its final report.
    // Battle-return cleanup clears those facts and restores the galaxy UI;
    // the owned scene children instead leave with State after this report.
    if (state.tactical) state.tactical->close_requested();
    state.tactical.reset();
    if (state.game) {
        state.game->stop();
        state.game.reset();
        state.owner_expired = state.owner_lifetime.expired();
    }
    if (state.completed) return;
    state.completed = true;
    state.status = state.failure.empty() ? "closed" : "failed";
    static_cast<void>(state.write_report());
}

} // namespace eawr::presentation::godot_backend
