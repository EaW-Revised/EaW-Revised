#include "galactic/galactic_mode_internal.hpp"
#include "galactic/galactic_hud_fixture.hpp"
#include "galactic/live_battle_bounty.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include "eawr/platform/campaign_end_inputs.hpp"
#include "eawr/platform/campaign_scoring.hpp"
#include "eawr/platform/campaign_service_configuration.hpp"
#include "eawr/units/galactic_types.hpp"
#include "eawr/sim/galactic/victory.hpp"
#include "black_market_hud_fixture.hpp"
#include "capture_hud_fixture.hpp"
#include "clock_capture_pair.hpp"
#include "corruption_hud_fixture.hpp"
#include "destruction_hud_fixture.hpp"
#include "fleet_hud_fixture.hpp"
#include "ground_hud_fixture.hpp"
#include "hero_action_hud_fixture.hpp"
#include "hud_clock_fixture.hpp"
#include "history_hud_fixture.hpp"
#include "orbit_hud_fixture.hpp"
#include "stock_hero_place.hpp"
#include "weapon_action_hud_fixture.hpp"
#include "weapon_action_live_fixture.hpp"
#include <godot_cpp/classes/engine.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <tuple>
#include <utility>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
// Explicit synthetic qualification names: score-<win|loss>-<empire|rebel|underworld>
// with optional -land / -space (fixed picture draw), -bare (no scoring provider) and -ledger
// (the production provider: a scripted production and ownership history is scored by the
// mounted authored module, GHUD-S61).
struct ScoreFixture {
    bool valid{}, win{}, bare{}, ledger{};
    std::string faction, opponent;
    std::optional<bool> space;
};
ScoreFixture parse_score_fixture(std::string_view name) {
    ScoreFixture fixture;
    if (!name.starts_with("score-")) return fixture;
    name.remove_prefix(6);
    const auto next = [&name] {
        const auto dash = name.find('-');
        const auto word = name.substr(0, dash);
        name.remove_prefix(dash == std::string_view::npos ? name.size() : dash + 1);
        return word;
    };
    const auto result = next();
    if (result != "win" && result != "loss") return fixture;
    fixture.win = result == "win";
    const auto faction = next();
    if (faction == "empire") { fixture.faction = "EMPIRE"; fixture.opponent = "REBEL"; }
    else if (faction == "rebel") { fixture.faction = "REBEL"; fixture.opponent = "EMPIRE"; }
    else if (faction == "underworld") { fixture.faction = "UNDERWORLD"; fixture.opponent = "EMPIRE"; }
    else return fixture;
    while (!name.empty()) {
        const auto option = next();
        if (option == "land") fixture.space = false;
        else if (option == "space") fixture.space = true;
        else if (option == "bare") fixture.bare = true;
        else if (option == "ledger") fixture.ledger = true;
        else return {};
    }
    fixture.valid = true;
    return fixture;
}
}

class CaptureHudFixture final : public GalacticHudFixture {
public:
    explicit CaptureHudFixture(const GalacticMode::Options& options)
        : name_(options.hud_fixture), synthetic_(options.campaign == galactic_view::synthetic_campaign) {}
    bool enabled(Feature feature) const override;
    std::optional<presentation::ui::PixelRect> ground_stack_rect(GalacticMode::State&, std::string_view,
        std::size_t, const presentation::ui::ShellPlacement&) const override;
    std::optional<bool> score_space() const override;
    std::optional<RotationInput> rotation_input(std::uint64_t frame) const override;
    bool configure_scene(GalacticMode::State& state, galactic_view::GalaxySnapshotStub& snapshot) override;
    bool configure_setup(GalacticMode::State& state) override;
    bool start_session(GalacticMode::State& state) override;
    bool prepare_ability(GalacticMode::State& state) override;
    bool black_market(GalacticMode::State& state) override;
    bool destruction(GalacticMode::State& state) override;
    bool hero_action(GalacticMode::State& state) override;
    bool weapon_action(GalacticMode::State& state) override;
    bool corruption(GalacticMode::State& state) override;
    bool orbit(GalacticMode::State& state) override;
    bool configure_hud(GalacticMode::State& state) override;
    void update_hud(GalacticMode::State& state) override;
    void observation(GalacticMode::State& state, sim::galactic::ObservedPlanet& observation, sim::galactic::ObserverPlanet& fixture_planet, sim::galactic::PlanetProduction& fixture_production, std::optional<double>& basic_income, std::optional<double>& actual_income, galactic_view::ObjectId planet) override;
    std::optional<bool> choose_battle(GalacticMode::State& state, std::uint64_t id, galactic_view::BattleChoice choice) override;
    bool battle_dialog(GalacticMode::State& state) override;
    void publish_battle(GalacticMode::State& state) override;
    void corruption_frame(GalacticMode::State& state, double elapsed) override;
    std::optional<std::uint64_t> corruption_display_frame() const override;
    bool configure_score(GalacticMode::State& state, platform::LiveGameSession::Options& live) override;
    bool run_score_ledger(GalacticMode::State& state) override;
private:
    void stage_steal(GalacticMode::State& state);
    void stage_ability(GalacticMode::State& state);
    void publish_ability_hero(GalacticMode::State& state);
    std::string name_;
    bool synthetic_{};
    sim::galactic::PlanetId steal_target_{};
    bool steal_published_{};
    int ability_stage_{};
    sim::galactic::PlanetId ability_target_{};
    sim::galactic::PlanetId ability_home_{};
    sim::galactic::PlanetId ability_other_{};
    double corruption_fixture_seconds{};
    std::weak_ptr<const void> corruption_fixture_owner_;
    bool corruption_fixture_owner_seen_{};
    bool corruption_fixture_loaded_{};
    bool corruption_fixture_repaused_{};
    galactic_view::BattleDialogView battle_dialog_fixture;
    bool score_ledger_script{};
    std::unique_ptr<ClockCapturePair> clock_pair_;
};

bool CaptureHudFixture::enabled(Feature feature) const {
    if (!synthetic_) return false;
    switch (feature) {
    case Feature::fleet_visibility: return galactic_view::fleet_hud_fixture(name_);
    case Feature::ground_placement: return galactic_view::ground_fixture(name_);
    case Feature::intelligence: return name_ == "known" || name_ == "masked" || name_ == "known-zero" || name_ == "discovered-only" || name_ == "known-none";
    case Feature::unaligned_planets: return name_ == "orbit-placement-yavin-overview-before";
    case Feature::driven_transit: return name_ == "fleet-transit" || name_ == "fleet-transit-plain";
    case Feature::battle_dialog: return name_.starts_with("battle-");
    case Feature::corruption_overview: return name_.starts_with("corruption-spread");
    }
    return false;
}
std::optional<bool> CaptureHudFixture::score_space() const {
    const auto fixture = synthetic_ ? parse_score_fixture(name_) : ScoreFixture{};
    return fixture.valid ? fixture.space : std::nullopt;
}
std::optional<GalacticHudFixture::RotationInput> CaptureHudFixture::rotation_input(std::uint64_t frame) const {
    if (!synthetic_ || !name_.starts_with("orbit-rotation-")) return {};
    constexpr std::array<double, 5> elapsed{20.0, 0.0, 0.001, 0.032, 0.033};
    return RotationInput{frame >= 1 && frame <= elapsed.size() ? elapsed[frame - 1] : 0.0, name_.ends_with("-zero")};
}

bool CaptureHudFixture::configure_scene(GalacticMode::State& state, galactic_view::GalaxySnapshotStub& snapshot) {
    auto& options = state.options;
    auto& galaxy_setup = state.galaxy_setup;
    auto& map_input = state.map_input;
    if (!synthetic_ && name_ == "cache-corruption-load") {
        // Prepare the real campaign owner with an explicitly clean fixture.
        // Changing galaxy_setup later cannot affect its already initialized root.
        if (!state.filesystem) { state.failure = "Corruption load fixture needs installed data"; return false; }
        auto catalog = state.audio_catalog;
        if (!catalog) {
            auto loaded = data::load_catalog_strict(*state.filesystem, state.profile == "remake" ? data::Profile::remake
                : (state.profile == "foc" ? data::Profile::foc : data::Profile::eaw));
            if (!loaded) { state.failure = core::format_diagnostic(loaded.error()); return false; }
            catalog = std::make_shared<const data::Catalog>(std::move(loaded).value().catalog);
        }
        auto tables = data::galactic::load_galactic_data(*state.filesystem, *catalog);
        if (!tables) { state.failure = core::format_diagnostic(tables.error()); return false; }
        data::ValueOverride clean{options.campaign, "Corruption_Level_Override", {}};
        for (const auto& planet : tables.value().planets) clean.values.push_back(planet.name + " 0");
        const std::array overrides{std::move(clean)};
        auto overridden = data::with_overrides(*catalog, overrides);
        if (!overridden) { state.failure = core::format_diagnostic(overridden.error()); return false; }
        state.audio_catalog = std::make_shared<const data::Catalog>(std::move(overridden).value());
    }
    if (options.campaign == galactic_view::synthetic_campaign)
        galactic_view::configure_orbit_scene_fixture(snapshot, options.hud_fixture);
    if (options.campaign == galactic_view::synthetic_campaign && (options.hud_fixture == "radar-locked"
        || options.hud_fixture == "corruption-locked"))
        map_input.gates.tutorial_locked = true; // Explicit runtime fixture, never live campaign permissions.
    if (options.campaign == galactic_view::synthetic_campaign && !options.synthetic_battle && !options.hud_fixture.empty())
        galactic_view::configure_synthetic_hud_clock(galaxy_setup);

    // #6756: the planets' weather icon is a stock command-bar atlas entry (atlas) or a standalone stock texture that is
    // not an atlas entry, like a mod's loading picture (loadscreen); the planet card and the opened map must only draw the first.
    if (synthetic_ && (name_ == "ground-weather-atlas" || name_ == "ground-weather-loadscreen"))
        for (auto& planet : snapshot.planets)
            planet.weather.icon = name_ == "ground-weather-atlas" ? "i_pa_weather_sun.tga" : "load_weather_sun.tga";

    if (synthetic_ && name_.starts_with("clock-mp-")) {
        const bool coop = name_ == "clock-mp-coop";
        const std::uint32_t other = coop ? 2U : 3U;
        options.seat = name_ == "clock-mp-host" ? 1U : other;
        for (auto& seat : snapshot.seats) if (seat.human && seat.id != 1U && seat.id != other) {
            seat.human = false; seat.active = coop && seat.id == 3U;
        }
        for (auto& seat : galaxy_setup.seats) if (seat.kind == sim::SeatKind::human && seat.seat_id.value != 1U && seat.seat_id.value != other) {
            seat.kind = sim::SeatKind::ai; seat.active = coop && seat.seat_id.value == 3U; seat.human_side_id = 0;
        }
    }

    return true;
}

bool CaptureHudFixture::configure_setup(GalacticMode::State& state) {
    auto& options = state.options;
    auto& failure = state.failure;
    auto& galaxy = state.galaxy;
    auto& galaxy_setup = state.galaxy_setup;
    auto& fleet_display_traits = state.fleet_display_traits;
    auto& fleet_visibility = state.fleet_visibility;
    auto& production_names = state.production_names;
    auto& audio_catalog = state.audio_catalog;
    auto& content = state.content;
    if (synthetic_ && name_ == "audio-mixer" && audio_catalog) {
        // In-memory qualification data only; installed XML and mods stay untouched.
        std::vector<data::ValueOverride> overrides;
        for (const auto* faction : {"Rebel", "Empire"}) {
            overrides.push_back({faction, "SFXEvent_Planet_Gained_Control", {"Unit_Boron_Missile_Fire"}});
            overrides.push_back({faction, "SFXEvent_Planet_Lost_Control", {"Unit_Attacked_C3PO"}});
        }
        auto overridden = data::with_overrides(*audio_catalog, overrides);
        if (!overridden) { failure = core::format_diagnostic(overridden.error()); return false; }
        audio_catalog = std::make_shared<const data::Catalog>(std::move(overridden).value());
    }
    if (synthetic_ && name_ == "history")
        galactic_view::configure_history_hud_fixture(galaxy_setup, galaxy->local.player);
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::fleet_hud_fixture(options.hud_fixture)
        && !galactic_view::ground_fixture(options.hud_fixture)) {
        galactic_view::configure_fleet_hud_fixture(galaxy_setup, galaxy->local.player, options.hud_fixture,
            fleet_display_traits, fleet_visibility, production_names);
        if (options.hud_fixture.starts_with("fleet-drag-palpatine")) {
            production_names[1004] = "Shuttle_Tyderium";
            production_names[1000] = "Emperor_Palpatine";
            galaxy_setup.movement->contents = {{7001, {{7002,1000}}}};
            if (options.hud_fixture == "fleet-drag-palpatine-mixed")
                galaxy_setup.fleets[0].members.push_back({7003,1002});
        }
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
    const bool stealth_move = options.hud_fixture == "fleet-drag-stealth";
    if (options.campaign == galactic_view::synthetic_campaign && (options.hud_fixture == "fleet-drag-home" || stealth_move)) {
        // Explicit native Home producer fixture: no configured home, first leader at another world.
        if (!stealth_move) galaxy_setup.fleets.front().orbit = 102;
        else {
            // #5093 / WGM-23: live combat skipping must permit the return
            // route to expand from this defended destination after arrival.
            galaxy_setup.fleets.front().members.resize(1);
            for (auto& row : galaxy_setup.movement->access)
                if (row.player == galaxy->local.player && row.planet == 102) row.potential_conflict = true;
        }
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
        if (stealth_move) {
            auto& hero = census.objects.front();
            hero.abilities.present = hero.abilities.combat_skip = true;
        }
        if (content && !stealth_move) {
            content->home_types.erase(galaxy->local.player);
            content->leader_types[galaxy->local.player] = production_names.at(census.objects.front().current_type);
        }
    }
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::ground_fixture(options.hud_fixture))
        galactic_view::configure_ground_fixture(galaxy_setup,galaxy->local.player,options.hud_fixture,
            fleet_display_traits,fleet_visibility,production_names);
    // GPD-02/14/22: the land-tray fixture gives synthetic planet 101 a stock land map, so the advanced map has unit pads.
    // Synthetic content carries no planet art; the map name is the stock one for Coruscant.
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "ground-tray" && content)
        content->art[101].land_tactical_map = "_Land_Planet_Coruscant_02.ted";
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::weapon_action_live_fixture(options.hud_fixture))
        galactic_view::configure_weapon_action_live_fixture(galaxy_setup, galaxy->local.player,
            fleet_display_traits, fleet_visibility, production_names, options.hud_fixture);
    if (synthetic_ && name_ == "destruction-live-effects-missing-model") {
        // A refused ordinary upload must not hide the valid, preloaded destroyed-state model after ruins.
        constexpr auto missing = "Data/Art/Models/Synthetic_Missing_Planet.alo";
        for (auto& planet : state.metadata.planets) if (planet.id == 101) planet.model = missing;
        for (auto& planet : galaxy->planets) if (planet.id == 101) planet.model = missing;
    }
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
    }
    return true;
}

bool CaptureHudFixture::start_session(GalacticMode::State& state) {
    auto& options = state.options;
    auto& failure = state.failure;
    auto& galaxy = state.galaxy;
    auto& game = state.game;
    if (synthetic_ && name_ == "history") {
        const auto publish = [&](sim::galactic::CommandPayload payload) {
            const auto target = game->frame().session_tick + 1;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(payload), {}});
            game->advance_to(target);
            return game->wait_for(target, std::chrono::seconds(5));
        };
        if (options.paused && !publish(sim::galactic::ResumeGalaxy{})) { failure = "History fixture could not resume"; return false; }
        const auto target = game->frame().session_tick + 61;
        game->advance_to(target);
        if (!game->wait_for(target, std::chrono::seconds(10)) || (options.paused && !publish(sim::galactic::PauseGalaxy{}))) {
            failure = "History fixture did not publish its admitted samples"; return false;
        }
    }
    if (enabled(Feature::driven_transit)) {
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

    return true;
}

bool CaptureHudFixture::prepare_ability(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& game = state.game;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::weapon_action_live_fixture(options.hud_fixture)
        && !galactic_view::weapon_action_entry_fixture(options.hud_fixture)) {
        // Live acceptance fixture: the ability menu is opened by a real seat command; the HUD then queues the host's
        // typed capability (never a copied menu). Real-time pacing processes it even while the galaxy is paused.
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::PrepareGalacticAbility{
            galactic_view::live_hack_actor, galactic_view::live_hack_planet,
            galactic_view::weapon_action_live_kind(options.hud_fixture)}, {}});
    }

    return true;
}

bool CaptureHudFixture::black_market(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& market_menu = state.market_menu;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::black_market_fixture(options.hud_fixture))
        market_menu.fixture = galactic_view::black_market_fixture_snapshot(galaxy->local.player, options.hud_fixture);

    return true;
}

bool CaptureHudFixture::destruction(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& destruction_hud = state.destruction_hud;
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture.starts_with("destruction-")
        && !options.hud_fixture.starts_with("destruction-live"))
        destruction_hud.fixture = galactic_view::destruction_fixture(galaxy->local.player, options.hud_fixture);

    return true;
}

bool CaptureHudFixture::hero_action(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& production_names = state.production_names;
    auto& hero_menu = state.hero_menu;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::hero_action_fixture(options.hud_fixture)) {
        hero_menu.fixture = galactic_view::hero_action_fixture_snapshot(galaxy->local.player, options.hud_fixture);
        production_names.insert({{1000,"Rebel_Trooper_Team"},{1001,"Corellian_Corvette"},
            {1002,"Obi_Wan_Team"},{1003,"Smuggler_Team_E"},{1004,"Smuggler_Team_R"}});
    }

    return true;
}

bool CaptureHudFixture::weapon_action(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& weapon_menu = state.weapon_menu;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::weapon_action_fixture(options.hud_fixture))
        weapon_menu.fixture = galactic_view::weapon_action_fixture_snapshot(galaxy->local.player, options.hud_fixture);

    return true;
}

bool CaptureHudFixture::corruption(GalacticMode::State& state) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& production_names = state.production_names;
    auto& corruption_menu = state.corruption_menu;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::corruption_fixture(options.hud_fixture)) {
        corruption_menu.fixture = galactic_view::corruption_fixture_snapshot(galaxy->local.player, options.hud_fixture);
        production_names.insert({{1000,"Rebel_Trooper_Team"},{1001,"Corellian_Corvette"},
            {2000,"Coruscant"},{2001,"Naboo"}});
        corruption_menu.briefing_keys = {{90,"TEXT_CORRUPTION_CORUSCANT_STORY_BRIBERY"},
            {91,"TEXT_CORRUPTION_CORUSCANT_STORY_BLACKMARKET"}};
    }

    return true;
}

bool CaptureHudFixture::orbit(GalacticMode::State& state) {
    auto& options = state.options;
    auto& command_catalog = state.command_catalog;
    auto& fleet_anchors = state.fleet_anchors;
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "orbit-extra") {
        // Explicit synthetic authored-component fixture: a third mesh/component pair.
        const auto* anchor = fleet_anchors.find("space_Pad_02");
        const auto* component = command_catalog.find("space_Pad_02");
        if (anchor && component) {
            auto extra = *anchor; extra.name = "space_Pad_03"; extra.rect.x -= extra.rect.width;
            fleet_anchors.add(std::move(extra));
            auto extra_component = *component; extra_component.name = "space_Pad_03";
            static_cast<void>(command_catalog.add(std::move(extra_component)));
        }
    }

    return true;
}

// "steal-stock" on a named campaign: an ordinary move stands in for the player's own drag to an Underworld planet (no
// ability, no prepared state, nothing listed by hand). The rest of the owner's steps stay real mouse input. When the
// team orbits there, the driver is told which icon to drag.
void CaptureHudFixture::stage_steal(GalacticMode::State& state) {
    if (steal_published_ || !state.game || !state.galaxy || !state.published_snapshot || !state.content) return;
    const auto& snapshot = *state.published_snapshot;
    const auto player = state.galaxy->local.player;
    const auto bearer = std::find_if(state.production_names.begin(), state.production_names.end(),
        [](const auto& row) { return row.second == "Droid_R2D2"; });
    if (bearer == state.production_names.end()) return;
    const auto place = eawr::testing::galactic::find_hero_place(snapshot, player, bearer->first);
    if (!place) return;
    if (!steal_target_) {
        const auto owners = std::find_if(state.content->snapshot.players.begin(), state.content->snapshot.players.end(),
            [](const auto& row) { return row.faction == "Underworld"; });
        if (owners == state.content->snapshot.players.end()) return;
        const auto planets = state.input_planets();
        std::vector<sim::galactic::PlanetId> route;
        for (const auto& planet : planets) {
            if (planet.allegiance != owners->id) continue;
            const auto path = galactic_view::destination_path(snapshot, player, place->planet, planet.id, planets);
            if (path && (route.empty() || path->size() < route.size())) route = *path;
        }
        if (route.empty()) return; // Waits for the observer, as the map does.
        std::optional<sim::galactic::CommandPayload> move;
        if (place->fleet) {
            if (const auto intent = galactic_view::fleet_destination_intent(snapshot, player, place->dragged, route.back(), planets, {}))
                move = state.map_input.command(*intent, planets);
        } else move = sim::galactic::LaunchFleet{{place->dragged}, route, std::nullopt, 0};
        if (!move) return;
        steal_target_ = route.back();
        state.game->submit(platform::LiveGameOrder{sim::SeatId{state.galaxy->local.seat}, std::move(*move), {}});
        return;
    }
    if (!place->fleet || place->transit || place->planet != steal_target_) return;
    for (const auto& [key, badge] : state.fleet_badges) {
        if (key.first != steal_target_ || !badge.button || badge.owner != player || !badge.button->is_visible_in_tree()) continue;
        steal_published_ = true;
        Engine::get_singleton()->set_meta("eawr_steal_stage", String::num_uint64(steal_target_) + " " + String(badge.button->get_name()));
        return;
    }
}

// "ability-stock:<member type>[:<enemy faction>[:<planet name>[:panel]]]" on a named campaign (#6909): nothing is staged but the
// report of where the drag starts. The hero named by the fixture (placed by the test's own data overlay or bought with the
// purchase hook) is found among the published actors. The drop target is the named planet, else the nearest planet held by
// another non-neutral player (of the second part's faction, if given). Once the hero's own stack icon is on screen the driver
// is told the target planet and the icon to press (and again, as a second stage, when the hero stands over the target); the
// drag, the tile, the release and the menu are real input, like the owner's. A fourth part "panel" drags the hero's own cell in the
// selected planet's fleet panel instead of its stack icon. `eawr_ability_why` names what is still missing.
// #7357: every frame, what the driver needs after the menu closed, read from the viewer's own state: the hero's planet,
// whether it travels, the selected fleet and the hero's fleet, the planet it started from and its screen point, the selected
// planet, the name of the hero's cell in that planet's fleet panel when one is shown ("-" otherwise), the screen point of
// the hero's planet, and another planet B to send it on to with its screen point. Nothing is changed.
void CaptureHudFixture::publish_ability_hero(GalacticMode::State& state) {
    if (!state.galaxy || !state.published_snapshot) return;
    std::string member = name_.substr(std::string_view("ability-stock:").size());
    if (const auto colon = member.find(':'); colon != std::string::npos) member.resize(colon);
    const auto bearer = std::find_if(state.production_names.begin(), state.production_names.end(),
        [&](const auto& row) { return row.second == member; });
    if (bearer == state.production_names.end()) return;
    const auto place = eawr::testing::galactic::find_hero_place(*state.published_snapshot, state.galaxy->local.player, bearer->first);
    if (!place) { Engine::get_singleton()->set_meta("eawr_ability_hero", String("none")); return; }
    if (!ability_home_ && place->fleet && !place->transit) ability_home_ = place->planet;
    const auto point = [&](sim::galactic::PlanetId planet) -> String {
        const auto anchor = state.hud_planet_anchors.find(planet);
        return anchor == state.hud_planet_anchors.end() ? String("- -") : String::num(anchor->second.x) + " " + String::num(anchor->second.y);
    };
    const auto home = point(ability_home_), here = point(place->planet);
    // The owner's acceptance planet B: the nearest other planet (not the start, not the one the hero stands at) the hero's
    // fleet can be ordered to by the viewer's own move rule (its combat skip lets a stealth hero leave a defended planet), a
    // hostile one first, as the owner sent the Defiler on to another enemy planet, and one on screen first, as a player drags
    // to a planet they can see. "0" when there is none, never a stale one.
    if (!place->transit && place->fleet) {
        const auto planets = state.input_planets();
        const auto* control = state.published_snapshot->control();
        const auto shown = [&](sim::galactic::PlanetId planet) {
            const auto anchor = state.hud_planet_anchors.find(planet);
            return anchor != state.hud_planet_anchors.end() && anchor->second.x >= 8 && anchor->second.y >= 8
                && anchor->second.x < static_cast<float>(state.camera.width) - 8 && anchor->second.y < static_cast<float>(state.camera.height) - 8;
        };
        sim::galactic::PlanetId other{};
        std::size_t best{}; bool best_hostile{}, best_shown{};
        for (const auto& planet : state.published_snapshot->planets()) {
            if (planet.id == ability_home_ || planet.id == place->planet) continue;
            const auto move = galactic_view::fleet_destination_intent(*state.published_snapshot, state.galaxy->local.player,
                place->dragged, planet.id, planets, {});
            if (!move) continue;
            bool hostile = planet.owner && planet.owner != state.galaxy->local.player;
            if (control) for (const auto& row : control->players) if (row.player == planet.owner && row.neutral) hostile = false;
            const bool visible = shown(planet.id);
            const auto rank = std::tuple{!visible, !hostile, move->path.size()};
            if (!other || rank < std::tuple{!best_shown, !best_hostile, best}) {
                other = planet.id; best = move->path.size(); best_hostile = hostile; best_shown = visible;
            }
        }
        ability_other_ = other;
    }
    String cell = "-";
    for (std::size_t index = 0; index < state.shown_panel_cards.size() && cell == "-"; ++index) {
        const auto* card = state.shown_panel_cards[index];
        if (!card) continue;
        const auto& owners = state.fleet_cell_layouts[index].owners;
        for (std::size_t slot = 0; slot < owners.size() && slot < state.fleet_group_buttons[index].size(); ++slot) {
            if (!owners[slot] || *owners[slot] >= card->groups.size() || card->groups[*owners[slot]].fleet != place->dragged) continue;
            const auto button = state.fleet_buttons.find(state.fleet_group_buttons[index][slot]);
            if (button != state.fleet_buttons.end() && button->second && button->second->is_visible_in_tree()) { cell = button->second->get_name(); break; }
        }
    }
    Engine::get_singleton()->set_meta("eawr_ability_hero", String::num_uint64(place->planet) + " " + String::num_uint64(place->transit ? 1 : 0)
        + " " + String::num_uint64(state.selected_fleet) + " " + String::num_uint64(place->dragged) + " " + String::num_uint64(ability_home_)
        + " " + home + " " + String::num_uint64(state.selected_planet) + " " + cell + " " + here
        + " " + String::num_uint64(ability_other_) + " " + point(ability_other_));
}
void CaptureHudFixture::stage_ability(GalacticMode::State& state) {
    publish_ability_hero(state);
    if (ability_stage_ >= 2 || !state.game || !state.galaxy || !state.published_snapshot || !state.content) return;
    const auto why = [](const char* text) { Engine::get_singleton()->set_meta("eawr_ability_why", String(text)); };
    const auto& snapshot = *state.published_snapshot;
    const auto player = state.galaxy->local.player;
    std::string member = name_.substr(std::string_view("ability-stock:").size()), enemy, hint;
    bool panel = false;
    if (const auto colon = member.find(':'); colon != std::string::npos) { enemy = member.substr(colon + 1); member.resize(colon); }
    if (const auto colon = enemy.find(':'); colon != std::string::npos) { hint = enemy.substr(colon + 1); enemy.resize(colon); }
    if (const auto colon = hint.find(':'); colon != std::string::npos) { panel = hint.substr(colon + 1) == "panel"; hint.resize(colon); }
    const auto bearer = std::find_if(state.production_names.begin(), state.production_names.end(),
        [&](const auto& row) { return row.second == member; });
    if (bearer == state.production_names.end()) return why("no type of that name");
    const auto place = eawr::testing::galactic::find_hero_place(snapshot, player, bearer->first);
    if (!place) return why("the hero is not on the map yet");
    if (place->transit || !place->fleet) return why("the hero is in transit or not in a fleet");
    // The second stage: the hero stands over the drop planet after its travel, for a drop on the planet it is parked at.
    if (ability_stage_ == 1 && place->planet != ability_target_) return why("waiting for the hero to reach the target");
    const auto* control = snapshot.control();
    if (!control) return why("no control service");
    if (ability_stage_ == 0) {
        const auto planets = state.input_planets();
        if (!hint.empty()) {
            for (const auto& planet : state.galaxy->planets) {
                const bool same = planet.name.size() == hint.size() && std::equal(hint.begin(), hint.end(), planet.name.begin(),
                    [](char left, char right) { return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right)); });
                if (same) ability_target_ = planet.id;
            }
        } else {
            std::size_t length{};
            for (const auto& planet : snapshot.planets()) {
                const auto owner = std::find_if(control->players.begin(), control->players.end(),
                    [&](const auto& row) { return row.player == planet.owner; });
                if (!planet.owner || planet.owner == player || owner == control->players.end() || owner->neutral) continue;
                if (!enemy.empty()) {
                    const auto held = std::find_if(state.content->snapshot.players.begin(), state.content->snapshot.players.end(),
                        [&](const auto& row) { return row.id == planet.owner; });
                    if (held == state.content->snapshot.players.end() || held->faction != enemy) continue;
                }
                const auto path = galactic_view::destination_path(snapshot, player, place->planet, planet.id, planets);
                if (path && (!ability_target_ || path->size() < length)) { ability_target_ = planet.id; length = path->size(); }
            }
        }
        if (!ability_target_) return why("no drop planet");
    }
    if (panel) {
        // The planet panel's own cell for the hero: select the planet, then find the cell of the group that is the hero's fleet.
        if (state.selected_planet != place->planet) {
            state.apply_selection(state.map_input.select(state.selection_planets(), place->planet, state.seat_flight().state()));
            return why("selecting the hero's planet");
        }
        for (std::size_t index = 0; index < state.shown_panel_cards.size(); ++index) {
            const auto* card = state.shown_panel_cards[index];
            if (!card) continue;
            const auto& owners = state.fleet_cell_layouts[index].owners;
            for (std::size_t cell = 0; cell < owners.size() && cell < state.fleet_group_buttons[index].size(); ++cell) {
                if (!owners[cell] || *owners[cell] >= card->groups.size() || card->groups[*owners[cell]].fleet != place->dragged) continue;
                const auto button = state.fleet_buttons.find(state.fleet_group_buttons[index][cell]);
                if (button == state.fleet_buttons.end() || !button->second || !button->second->is_visible_in_tree()) continue;
                Engine::get_singleton()->set_meta(ability_stage_ == 0 ? "eawr_ability_stage" : "eawr_ability_stage2",
                    String::num_uint64(ability_target_) + " " + String::num_uint64(place->planet) + " " + String(button->second->get_name()));
                ++ability_stage_;
                return;
            }
        }
        return why("no visible panel cell for the hero's fleet");
    }
    // The hero's own stack icon (a shared icon when it stands in an ordinary slot): the one whose fleet it is, else the
    // icon of its orbit slot, which is what the player presses.
    const auto fleets = snapshot.fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == place->dragged; });
    if (fleet == fleets.end()) return why("the hero's fleet is not published");
    const godot::TextureButton* chosen = nullptr;
    for (const auto& [key, badge] : state.fleet_badges) {
        if (key.first != place->planet || !badge.button || badge.owner != player || !badge.button->is_visible_in_tree()) continue;
        if (badge.fleet == place->dragged) { chosen = badge.button; break; }
        if (!chosen && state.stack_icon_slot(place->planet, key.second) == fleet->orbit_slot) chosen = badge.button;
    }
    if (!chosen) return why("no visible stack icon for the hero");
    Engine::get_singleton()->set_meta(ability_stage_ == 0 ? "eawr_ability_stage" : "eawr_ability_stage2",
        String::num_uint64(ability_target_) + " " + String::num_uint64(place->planet) + " " + String(chosen->get_name()));
    ++ability_stage_;
}

bool CaptureHudFixture::configure_hud(GalacticMode::State& state) {
    // The load qualification preserves the real campaign's HUD publications.
    if (!synthetic_ && name_ == "cache-corruption-load") return true;
    if (!synthetic_ && (name_ == "steal-stock" || name_.starts_with("ability-stock:"))) return true; // A stock campaign stays stock: no stub HUD facts.
    if (synthetic_ && name_ == "hero-map-respawn") return true; // Uses actual owner publications, not stub HUD facts.
    auto& options = state.options;
    auto& failure = state.failure;
    auto& map_input = state.map_input;
    auto& hud_facts = state.hud_facts;
    auto& selected_planet = state.selected_planet;
    auto& selected_hud = state.selected_hud;
    auto& base_levels_visible = state.base_levels_visible;
    auto& fast_forward_factor = state.fast_forward_factor;
    if (!options.hud_fixture.empty()) {
        if (options.campaign != galactic_view::synthetic_campaign
            || (options.hud_fixture != "accessible" && options.hud_fixture != "inaccessible"
                && options.hud_fixture != "objectives-audio"
                && options.hud_fixture != "audio-mixer"
                && options.hud_fixture != "known" && options.hud_fixture != "masked" && options.hud_fixture != "history"
                && options.hud_fixture != "known-zero" && options.hud_fixture != "discovered-only" && options.hud_fixture != "known-none"
                && options.hud_fixture != "production" && options.hud_fixture != "production-unlocks"
                && options.hud_fixture != "capture-live" && !options.hud_fixture.starts_with("clock-mp-") && !galactic_view::fleet_hud_fixture(options.hud_fixture)
                && options.hud_fixture != "radar-locked"
                && options.hud_fixture != "corruption" && options.hud_fixture != "corruption-clean"
                && !options.hud_fixture.starts_with("corruption-spread")
                && options.hud_fixture != "corruption-icons"
                && options.hud_fixture != "corruption-gaps" && options.hud_fixture != "corruption-locked"
                && options.hud_fixture != "corruption-no-mission-service" && options.hud_fixture != "corruption-republish"
                && options.hud_fixture != "theft"&& options.hud_fixture != "theft-single" && options.hud_fixture != "theft-many"
                && options.hud_fixture != "neutralization" && options.hud_fixture != "hack" && options.hud_fixture != "hack-poor"
                && options.hud_fixture != "hack-fractional"
                && !galactic_view::weapon_action_live_fixture(options.hud_fixture)
                && !galactic_view::black_market_fixture(options.hud_fixture)
                && options.hud_fixture != "battle-choice" && options.hud_fixture != "battle-land" && options.hud_fixture != "battle-progress"
                && options.hud_fixture != "battle-single" && options.hud_fixture != "battle-begin"
                && options.hud_fixture != "battle-refusal" && options.hud_fixture != "battle-observer"
                && !options.hud_fixture.starts_with("battle-summary")
                && options.hud_fixture != "battle-invasion" && options.hud_fixture != "battle-invasion-candidate"
                && !options.hud_fixture.starts_with("score-") && !options.hud_fixture.starts_with("orbit-"))) {
            failure = "HUD fixtures require synthetic and a supported intelligence/domain fixture";
            return false;
        }
        galactic_view::PlanetHudFacts fact;
        fact.planet = 101; fact.discovered = true; fact.display_name = "Synthetic Alpha";
        fact.name_colour = {185.0F / 255.0F, 40.0F / 255.0F, 39.0F / 255.0F};
        fact.surface_accessible = options.hud_fixture == "accessible";
        if (options.hud_fixture == "corruption-icons") {
            // WGCI-1: three corrupted planets that each publish a corruption type (sim types 6 bribery, 2 piracy, 5 corrupt militia).
            fact.corrupted = true; fact.corruption_type = 6;
            for (const auto& [id, type] : {std::pair<galactic_view::ObjectId, std::uint8_t>{102, 2}, {103, 5}}) {
                galactic_view::PlanetHudFacts other = fact;
                other.planet = id; other.corruption_type = type; other.display_name = "Synthetic Planet " + std::to_string(id);
                hud_facts.emplace(other.planet, other);
            }
        }
        hud_facts.emplace(fact.planet, fact);
        selected_planet = fact.planet;
        map_input.selected = fact.planet;
        if (options.hud_fixture == "production" || options.hud_fixture == "production-unlocks") selected_hud.space_tab = true;
        base_levels_visible = options.hud_fixture == "known" || options.hud_fixture == "masked"
            || options.hud_fixture == "known-zero" || options.hud_fixture == "discovered-only" || options.hud_fixture == "known-none";
        // A synthetic command-admission fixture, not the unverified profile map.
        fast_forward_factor = sim::math::Fixed::from_raw(2 * sim::math::Fixed::scale);
    }

    return true;
}

void CaptureHudFixture::update_hud(GalacticMode::State& state) {
    auto& options = state.options;
    auto& audio = state.audio;
    auto& command_catalog = state.command_catalog;
    auto& hud_updates = state.hud_updates;
    if (!synthetic_ && name_ == "cache-corruption-load" && state.game && corruption_fixture_owner_seen_
        && !corruption_fixture_repaused_
        && (corruption_fixture_owner_.owner_before(state.owner_lifetime)
            || state.owner_lifetime.owner_before(corruption_fixture_owner_))) {
        // Player-load deliberately drops saved pause tokens. Reapply an ordinary
        // seat pause before animation so the pixel witness keeps its held clock;
        // no camera/settings/visibility invalidation may clear the old picture.
        auto frame = state.game->frame();
        state.paused = frame.latest && frame.latest->clock().pause_depth != 0;
        if (!state.paused) {
            const auto target = frame.session_tick + 1;
            state.request_pause(true);
            // Restore opens this barrier after update_hud returns. The fixture
            // queues its pause first, then opens it before waiting for that input.
            state.game->presentation_ready();
            if (!state.game->wait_for(target, std::chrono::seconds(5))) {
                state.failure = "Corruption load fixture could not pause the restored owner"; return;
            }
            frame = state.game->frame();
            state.paused = frame.latest && frame.latest->clock().pause_depth != 0;
        }
        if (!state.paused) { state.failure = "Corruption load fixture restored owner is not paused"; return; }
        corruption_fixture_repaused_ = true;
    }
    if (!synthetic_ && name_ == "steal-stock") stage_steal(state);
    if (!synthetic_ && name_.starts_with("ability-stock:")) stage_ability(state);
    if (synthetic_ && name_ == "audio-mixer" && audio) {
        if (hud_updates == 5) audio->ui_sound("GUI_Dial_Switch_SFX");
        if (hud_updates == 10) audio->ui_sound("Unit_Fleet_Move_Star_Destroyer");
        if (hud_updates == 15) audio->sound(audio::GalacticSound::planet_gained, audio::Vec3{0, 0, 0});
        if (hud_updates == 20) audio->sound(audio::GalacticSound::planet_lost, audio::Vec3{0, 0, 0});
        if (hud_updates == 25) audio->build(audio::GalacticBuildCue::underway, "Death_Star", true);
    }
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "objectives-audio" && audio) {
        if (const auto* icon = command_catalog.find("objective_icon"); icon && hud_updates == 5) {
            audio->objectives_rebuilt(*icon, false);
            audio->objectives_rebuilt(*icon, true);
        }
        if (const auto* text = command_catalog.find("tutorial_text"); text && (hud_updates == 6 || hud_updates == 10))
            audio->teletype_revealed(*text);
    }

}

void CaptureHudFixture::observation(GalacticMode::State& state, sim::galactic::ObservedPlanet& observation, sim::galactic::ObserverPlanet& fixture_planet, sim::galactic::PlanetProduction& fixture_production, std::optional<double>& basic_income, std::optional<double>& actual_income, galactic_view::ObjectId planet) {
    auto& options = state.options;
    // Deliberately populated secret payloads also enter the masked
    // fixture: the same admission adapter must remove them entirely.
    // known-none (#6985): every discovered planet has no owner and no controller, and the seat can see that (control visible).
    const bool none = options.hud_fixture == "known-none";
    const bool known = options.hud_fixture == "known" || options.hud_fixture == "known-zero" || none;
    observation.discovered = planet != 103;
    observation.knowledge.ownership = known;
    observation.knowledge.ground_level = known;
    observation.knowledge.space_level = known;
    observation.knowledge.structures = known;
    observation.knowledge.political_control = known;
    observation.knowledge.income = known;
    basic_income = 75.0;
    actual_income = options.hud_fixture == "known-zero" ? 0.0 : 190.0;
    if (!none) observation.known_owner = 1;
    if (none) observation.apparent_allegiance_none = true;
    else if (known || options.hud_fixture == "discovered-only") {
        observation.apparent_owner = 1;
        observation.apparent_allegiance_owner = 1;
    }
    observation.known_political_control = sim::math::Fixed::from_raw(3 * sim::math::Fixed::scale);
    fixture_planet.planet = planet;
    fixture_planet.ground_level = options.hud_fixture == "known-zero" ? 0 : 4;
    fixture_planet.space_level = options.hud_fixture == "known-zero" ? 0 : 2;
    fixture_production.planet = planet;
    fixture_production.surface_accessible = true;
    fixture_production.ground_slots = 9; fixture_production.maximum_space_level = 5;
    if (options.hud_fixture != "known-zero") for (std::size_t index = 0; index < 5; ++index) {
        sim::galactic::ProductionObject object;
        object.domain = sim::galactic::ProductionDomain::ground;
        object.kind = sim::galactic::ProductionKind::structure;
        fixture_production.objects.push_back(object);
    }

}

std::optional<bool> CaptureHudFixture::choose_battle(GalacticMode::State& state, std::uint64_t id, galactic_view::BattleChoice choice) {
    auto& options = state.options;
    if (id != battle_dialog_fixture.conflict) {
        return std::nullopt;
    }
    using enum galactic_view::BattleChoice;
    if (choice == continue_galaxy || choice == invade) battle_dialog_fixture.phase = galactic_view::BattleDialogPhase::hidden;
    else if (choice == auto_resolve) {
        if (options.hud_fixture == "battle-refusal" && battle_dialog_fixture.response_revision == 0) {
            battle_dialog_fixture.refusal = "Conflict choice refused; try again.";
            ++battle_dialog_fixture.response_revision;
        } else battle_dialog_fixture.phase = galactic_view::BattleDialogPhase::progress;
    }
    else if (choice == retreat) battle_dialog_fixture.retreat_allowed = false;
    else if (choice == fight) battle_dialog_fixture.phase = galactic_view::BattleDialogPhase::hidden;
    return true;

}

bool CaptureHudFixture::battle_dialog(GalacticMode::State& state) {
    auto& options = state.options;
    auto& battle_text = state.battle_text;
    // Explicit synthetic presentation qualification, never live campaign facts.
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture.starts_with("battle-")) {
        auto& view = battle_dialog_fixture;
        view.conflict = 9001; view.planet = "Coruscant"; view.involved = options.hud_fixture != "battle-observer";
        view.pending_land = options.hud_fixture == "battle-land";
        view.phase = (options.hud_fixture == "battle-choice" || options.hud_fixture == "battle-refusal" || view.pending_land
            || options.hud_fixture == "battle-single" || options.hud_fixture == "battle-begin") ? galactic_view::BattleDialogPhase::choice
            : (options.hud_fixture.starts_with("battle-summary") || options.hud_fixture.starts_with("battle-invasion")) ? galactic_view::BattleDialogPhase::summary
            : galactic_view::BattleDialogPhase::progress;
        view.fight_allowed = true; view.auto_allowed = options.hud_fixture != "battle-single";
        view.single_button = !view.auto_allowed;
        view.tactical_available = options.hud_fixture == "battle-single" || options.hud_fixture == "battle-begin";
        view.retreat_allowed = view.phase == galactic_view::BattleDialogPhase::progress; view.won = true;
        if (options.hud_fixture == "battle-summary-audio-auto-win"
            || options.hud_fixture == "battle-summary-audio-auto-loss"
            || options.hud_fixture == "battle-summary-audio-fought") {
            view.auto_resolved = options.hud_fixture != "battle-summary-audio-fought";
            view.won = options.hud_fixture != "battle-summary-audio-auto-loss";
        }
        if (options.hud_fixture == "battle-invasion") { view.invasion_fleet = 80; view.invasion_planet = 101; }
        view.display_seconds = 10; view.elapsed_seconds = 2;
        view.sides[0] = {"Empire", "", {0.9F, 0.2F, 0.2F}, {"Imperial Star Destroyer", "TIE Fighter", "TIE Fighter", "TIE Bomber", "TIE Scout"}, {}, {}};
        view.sides[1] = {"Rebel Alliance", "", {0.2F, 0.9F, 0.2F}, {"Mon Calamari Cruiser", "X-wing"}, {}, {}};
        view.results.losses[0] = {{"TIE_Fighter", "TIE Fighter", 2}};
        view.results.losses[1] = {{"X_Wing", "X-wing", 3}};
        view.results.totals = {2, 3}; view.results.combat_power = {20, 60};
        if (options.hud_fixture == "battle-summary-zero") view.results.combat_power = {};
        if (options.hud_fixture == "battle-summary-half") view.results.combat_power = {7,1};
        if (options.hud_fixture == "battle-summary-bounty") {
            platform::LiveGameFrame frame;
            campaign::BattleResult result;
            result.conflict = view.conflict;
            result.bounty = sim::math::Fixed::from_raw(42 * sim::math::Fixed::scale);
            frame.result = std::move(result);
            view.bounty = galactic_view::live_campaign_bounty(frame, view.conflict, 1, battle_text);
        }
        if (options.hud_fixture == "battle-summary-corruption")
            view.bounty = galactic_view::CampaignBounty{false,true,"Collected bounty: 0"};
        if (options.hud_fixture == "battle-summary-auto-offer") {
            view.auto_resolved = true; view.invasion_fleet = 77; view.invasion_planet = 101;
        }
        if (options.hud_fixture == "battle-summary-slots") {
            view.results.losses[0].clear();
            for (std::size_t index = 0; index < 13; ++index)
                view.results.losses[0].push_back({"TIE_Fighter", "Group " + std::to_string(index), index == 0 ? 1U : 2U});
            for (std::size_t index = 0; index < 6; ++index)
                view.results.heroes[0].push_back({"TIE_Fighter", "Named " + std::to_string(index), index == 0 ? 2U : 1U});
            view.results.totals[0] = 32;
        }
    }

    return true;
}

void CaptureHudFixture::publish_battle(GalacticMode::State& state) {
    auto& options = state.options;
    auto& audio = state.audio;
    auto& battle_dialog = state.battle_dialog;
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture.starts_with("battle-")
        && battle_dialog_fixture != battle_dialog->view()) {
        // The HUD is constructed before audio. Stage this transition at its
        // first published fixture view, after the audio adapter exists.
        if (audio && !battle_dialog->view().conflict && options.hud_fixture.starts_with("battle-summary-audio-")) {
            audio->battle_pending(false);
            if (!battle_dialog_fixture.auto_resolved) { audio->battle_load(); audio->tactical(); }
        }
        battle_dialog->present(battle_dialog_fixture);
    }

}

void CaptureHudFixture::corruption_frame(GalacticMode::State& state, double elapsed) {
    auto& options = state.options;
    auto& galaxy = state.galaxy;
    auto& content = state.content;
    auto& publication = state.corruption_frame_publication;
    // #7357: the host republishes an unchanged menu while the dialog is open (a new snapshot every frame, same facts).
    // A click on the dialog must still land, so this fixture hands the HUD a fresh copy of the same menu every frame.
    if (synthetic_ && name_ == "corruption-republish" && state.corruption_menu.fixture)
        state.corruption_menu.fixture = galactic_view::corruption_fixture_snapshot(galaxy->local.player, name_);
    if (!synthetic_ && name_ == "cache-corruption-load") {
        if (!state.game) return; // Scene construction samples animation before starting the native owner.
        // Visual qualification only: never alter the native game/checkpoint.
        // The driver first captures a clean paused layer, then requests clouds
        // and lines, and finally loads that clean game through the real UI.
        if (!corruption_fixture_owner_seen_) {
            corruption_fixture_owner_ = state.owner_lifetime;
            corruption_fixture_owner_seen_ = true;
        }
        corruption_fixture_loaded_ = corruption_fixture_loaded_
            || corruption_fixture_owner_.owner_before(state.owner_lifetime)
            || state.owner_lifetime.owner_before(corruption_fixture_owner_);
        state.host->set_meta("eawr_cache_fixture_loaded", corruption_fixture_loaded_);
        state.host->set_meta("eawr_cache_fixture_native_corrupted", static_cast<std::int64_t>(
            std::count_if(publication.planets.begin(), publication.planets.end(),
                [](const auto& fact) { return fact.corrupted || fact.transitioning; })));
        const auto native = state.game->frame();
        state.host->set_meta("eawr_cache_fixture_native_pause_depth", static_cast<std::int64_t>(
            native.latest ? native.latest->clock().pause_depth : 0));
        if (corruption_fixture_loaded_ || !bool(state.host->get_meta("eawr_cache_fixture_corruption", false))) return;
        publication.planets.clear();
        publication.seat = galaxy->local.seat;
        for (const auto& planet : galaxy->planets) {
            galactic_view::CorruptionPlanetFact fact;
            fact.planet = planet.id; fact.corrupted = true;
            fact.particle_type = content->corruption_particle;
            fact.source_nonempty = fact.target_nonempty = true;
            publication.planets.push_back(std::move(fact));
        }
        // Warm only the injected effects while the native animation clock is
        // held. An animated background must not accidentally clear stale pixels.
        constexpr double visual_step = 1.0 / 30.0;
        corruption_fixture_seconds += visual_step;
        if (state.audio_catalog) {
            for (std::size_t layer = 0; layer < state.corruption_particles.size(); ++layer)
                if (auto& particles = state.corruption_particles[layer])
                    if (!particles->service(state.corruption_visuals, *state.audio_catalog, *content,
                            layer, state.camera, visual_step, state.failure)) return;
            if (state.corruption_lines && content->corruption_line_system)
                if (!state.corruption_lines->service_lines(state.corruption_visuals, *galaxy,
                        content->corruption_style, *content->corruption_line_system,
                        static_cast<std::uint64_t>(corruption_fixture_seconds * sim::galactic::galactic_frames_per_second),
                        state.camera, visual_step, state.failure)) return;
        }
        std::uint64_t particles = 0;
        for (const auto& owner : state.corruption_particles) if (owner) particles += owner->particle_count();
        state.host->set_meta("eawr_cache_fixture_particles", particles);
        state.host->set_meta("eawr_cache_fixture_lines", state.corruption_lines ? state.corruption_lines->particle_count() : 0);
        return;
    }
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture == "corruption") {
        publication.seat = galaxy->local.seat;
        galactic_view::CorruptionPlanetFact fact;
        fact.planet = 101; fact.corrupted = true; fact.particle_type = content->corruption_particle;
        fact.source_nonempty = fact.target_nonempty = true;
        publication.planets = {std::move(fact)};
    }
    corruption_fixture_seconds += std::max(0.0, elapsed);
    const bool spread_fixture = options.campaign == galactic_view::synthetic_campaign
        && options.hud_fixture.starts_with("corruption-spread");
    if (spread_fixture) {
        publication.seat = galaxy->local.seat; publication.planets.clear();
        for (const auto id : {101U, 102U, 103U}) {
            galactic_view::CorruptionPlanetFact fact;
            fact.planet = id; fact.corrupted = id != 102;
            fact.source_nonempty = fact.target_nonempty = fact.corrupted;
            fact.particle_type = content->corruption_particle;
            if (id == 102) {
                fact.transitioning = fact.increasing = true; fact.target_nonempty = true;
                fact.fraction = static_cast<float>(std::clamp(corruption_fixture_seconds/5, 0.0, 1.0));
                if (options.hud_fixture == "corruption-spread-settled" || fact.fraction >= 1) {
                    fact.corrupted = true; fact.transitioning = fact.increasing = false;
                    fact.source_nonempty = true; fact.fraction = 1;
                }
                if (options.hud_fixture == "corruption-spread-removing") {
                    fact.corrupted = true; fact.transitioning = true; fact.increasing = false;
                    fact.source_nonempty = true; fact.target_nonempty = false;
                }
                if (options.hud_fixture == "corruption-spread-removed") {
                    fact.corrupted = fact.transitioning = fact.increasing = false;
                    fact.source_nonempty = fact.target_nonempty = false;
                }
            }
            if (options.hud_fixture == "corruption-spread-replaced" && id == 102 && corruption_fixture_seconds >= 1)
                fact.particle_type = content->corruption_style.line_particle;
            publication.planets.push_back(std::move(fact));
        }
        if (options.hud_fixture == "corruption-spread-removed" && corruption_fixture_seconds >= 1)
            publication.planets.clear();
    }

}

std::optional<std::uint64_t> CaptureHudFixture::corruption_display_frame() const {
    if (name_ != "cache-corruption-load" && !enabled(Feature::corruption_overview)) return {};
    return static_cast<std::uint64_t>(corruption_fixture_seconds*sim::galactic::galactic_frames_per_second);
}

bool CaptureHudFixture::configure_score(GalacticMode::State& state, platform::LiveGameSession::Options& live) {
    auto& options = state.options;
    auto& failure = state.failure;
    auto& galaxy = state.galaxy;
    auto& galaxy_setup = state.galaxy_setup;
    auto& production_names = state.production_names;
    auto& audio_catalog = state.audio_catalog;
    auto& neutral_owner = state.neutral_owner;
    auto& filesystem = state.filesystem;

    const auto fixture = parse_score_fixture(options.hud_fixture);
    if (options.campaign != galactic_view::synthetic_campaign) return true;
    if (name_.starts_with("ground-invasion")) {
        // WGM-39/42: an invasion is requested only when a conflict owner is installed, as in every installed
        // campaign. Keep a real pending land conflict open to exercise the fleet cards behind its prompt.
        auto services = std::make_shared<platform::CampaignServiceConfiguration>();
        services->configure = [](campaign::GameSession& owner) {
            campaign::GameSession::ConflictLayerServices land;
            land.content = [](const campaign::ConflictRecord&) { return core::Result<sim::tactical::TacticalContent>::success({}); };
            land.script_requirement = campaign::ScriptRequirement::none;
            land.admission = [](const sim::galactic::GalacticSnapshot& snapshot, const sim::galactic::GalacticEvent& event, sim::EntityId next)
                -> core::Result<std::optional<campaign::RecordedAdmission>> {
                using Result = core::Result<std::optional<campaign::RecordedAdmission>>;
                if (event.kind != sim::galactic::GalacticEventKind::land_conflict_requested) return Result::success({});
                const auto fleets = snapshot.fleets();
                const auto fleet = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) { return row.id == event.subject; });
                if (fleet == fleets.end()) return Result::success({});
                campaign::ConflictRecord record;
                record.id = next; record.planet = event.value; record.layer = campaign::BattleLayer::land;
                record.invader = fleet->owner;
                record.defender = sim::galactic::surface_ground_holder(*snapshot.control(), event.value);
                record.map_identity = "synthetic-land";
                record.tactical.first_free_entity_id = next + 1;
                record.tactical.players = {{1, 1, 1, 1}, {2, 2, 2, 1}};
                record.seats = {{sim::galactic::SeatId{1}, 1}};
                for (const auto& member : fleet->members)
                    record.forces.push_back({member.id, member.type, fleet->owner, fleet->id});
                std::sort(record.forces.begin(), record.forces.end(), [](const auto& a, const auto& b) { return a.id < b.id; });
                return Result::success(campaign::RecordedAdmission{snapshot.completed_tick(), std::move(record),
                    {campaign::ConflictRoute::pending_tactical, false, false}});
            };
            return owner.install_conflict_layer(campaign::BattleLayer::land, std::move(land));
        };
        live.services = std::move(services);
        return true;
    }
    if (name_.starts_with("clock-mp-")) {
        clock_pair_ = std::make_unique<ClockCapturePair>();
        const sim::SeatId other{name_ == "clock-mp-coop" ? 2U : 3U};
        const net::PeerIndex local = name_ == "clock-mp-host" ? 0U : 1U;
        if (!clock_pair_->start(galaxy_setup, other, local, live, options.lan_match, failure)) return false;
    }
    if (options.hud_fixture == "hero-map-respawn") {
        // Explicit synthetic defeat inputs; the production owner schedules and publishes them.
        // No test-only material, progress uniform or direct viewer schedule injection.
        if (!audio_catalog || !galaxy) { failure = "hero return fixture needs installed art"; return false; }
        const auto local = galaxy->local.player;
        production_names[90001] = "Bossk";
        production_names[90002] = "Silri";
        sim::galactic::ObserverSetup observer; observer.human_observers = {local}; observer.solo_reference_human = local;
        for (const auto& player : galaxy_setup.players) observer.players.push_back({player.player_id});
        std::map<sim::galactic::TypeId, sim::galactic::ObserverType> traits;
        for (const auto& fleet : galaxy_setup.fleets) for (const auto& member : fleet.members)
            traits.try_emplace(member.type, sim::galactic::ObserverType{member.type});
        if (galaxy_setup.movement) for (const auto& container : galaxy_setup.movement->contents)
            for (const auto& member : container.children)
                traits.try_emplace(member.type, sim::galactic::ObserverType{member.type});
        for (const auto& [type, row] : traits) { static_cast<void>(type); observer.types.push_back(row); }
        for (const auto& planet : galaxy_setup.planets) {
            sim::galactic::ObserverPlanet facts; facts.planet = planet.id;
            facts.current_owner = facts.current_controller = facts.allegiance = facts.target_owner = planet.owner;
            observer.planets.push_back(facts);
        }
        for (const auto& player : galaxy_setup.players) for (const auto& planet : galaxy_setup.planets)
            observer.records.push_back({player.player_id, planet.id, true});
        galaxy_setup.observer = std::move(observer);
        sim::galactic::PlayerId other{};
        for (const auto& player : galaxy_setup.players)
            if (player.player_id != local && player.player_id != neutral_owner) { other = player.player_id; break; }
        if (!other) { failure = "hero return fixture needs a foreign owner"; return false; }
        auto services = std::make_shared<platform::CampaignServiceConfiguration>();
        services->configure = [local, other](campaign::GameSession& owner) {
            const sim::InlineExecutor executor;
            for (const auto player : {local, other}) {
                sim::galactic::HeroDefeat defeat;
                defeat.id = player == local ? 99001 : 99002;
                defeat.type = defeat.respawn_type = player == local ? 90001 : 90002;
                defeat.owner = player; defeat.named = true;
                defeat.seconds = sim::math::Fixed::from_raw(10 * sim::math::Fixed::scale);
                auto result = owner.schedule_hero_defeat(defeat, executor);
                if (!result) return core::Result<void>::failure(result.error());
                if (!result.value().scheduled) {
                    core::Diagnostic error; error.code = "fixture.hero_schedule";
                    error.message = "synthetic hero schedule was not admitted";
                    return core::Result<void>::failure(std::move(error));
                }
            }
            return core::Result<void>::success();
        };
        live.services = std::move(services);
        return true;
    }
    if (!fixture.valid) {
        if (options.hud_fixture.starts_with("score-")) failure = "unknown score fixture " + options.hud_fixture;
        return failure.empty();
    }
    if (!audio_catalog || !galaxy) { failure = "score fixtures require installed game data"; return false; }
    // A finished synthetic campaign: a matured credits victory for the winner, consumed
    // through the same owner path as a live campaign (WGS-82/176).
    const auto local = galaxy->local.player;
    if (fixture.ledger) {
        // Synthetic production offers (explicit fixture inputs, as the production HUD fixture)
        // and the debug owner command, so the history below is ordinary galaxy input.
        if (!galaxy_setup.fiscal || !galaxy_setup.player_economy) {
            failure = "ledger score fixture needs the synthetic clock";
            return false;
        }
        galactic_view::configure_synthetic_hud_production(galaxy_setup, local);
        galaxy_setup.debug_commands = true;
        production_names = {{1000, "TIE_Fighter_Squadron"}, {1001, "Empire_Star_Base_2"},
            {1002, "DS_Shield_Gen"}, {1003, "Stormtrooper_Team"}, {1004, "E_Ground_Barracks"}};
    }
    sim::galactic::PlayerId other = 0;
    for (const auto& player : galaxy_setup.players)
        if (player.player_id != local && player.player_id != neutral_owner) { other = player.player_id; break; }
    if (!other) { failure = "score fixtures require a second player"; return false; }
    sim::galactic::VictoryContext context;
    // Setup validation wants exactly one row per listed player, in listed order.
    for (const auto& player : galaxy_setup.players) {
        if (player.player_id == local) context.players.push_back({local, fixture.faction, false, true, true});
        else if (player.player_id == neutral_owner) context.players.push_back({neutral_owner, "NEUTRAL", true, false});
        else context.players.push_back({player.player_id, fixture.opponent, false, true, false, true});
    }
    for (const auto& planet : galaxy_setup.planets)
        context.planets.push_back({planet.id, 1001, planet.owner, planet.owner, planet.owner, false});
    context.types = {{"Fixture_Planet", 1001}};
    for (const auto& a : context.players) for (const auto& b : context.players)
        context.relations.push_back({a.player, b.player, a.player == b.player});
    sim::galactic::VictoryAuthoring authored;
    authored.human = {sim::galactic::VictoryKind::credits};
    authored.ai = authored.human;
    auto victory = sim::galactic::install_victory(authored, context, 1);
    if (!sim::galactic::register_victory(victory, sim::galactic::VictoryKind::credits,
        fixture.win ? local : other, 0, 0)) { failure = "score fixture victory was not accepted"; return false; }
    galaxy_setup.victory = std::move(victory);
    if (fixture.ledger) {
        // GHUD-S61: the production provider over the session's own saved ledger. Nothing here
        // is a stand-in; the history that fills the ledger is scripted in run_score_ledger().
        if (!filesystem) { failure = "ledger score fixtures require installed game data"; return false; }
        auto source = std::make_shared<platform::CampaignScoringSource>();
        source->files = filesystem;
        source->catalog = audio_catalog;
        source->types = std::make_shared<const units::GalacticTypeTable>(units::galactic_type_table(*audio_catalog));
        source->type_names = production_names;
        source->campaign_name = "Synthetic_Score";
        live.services = platform::campaign_end_services_scored(audio_catalog, std::move(source));
        score_ledger_script = true;
        return true;
    }
    // Synthetic scoring-provider stand-in (never campaign facts): a title key plus a
    // present value, an absent control and a present zero (WGP-97/103).
    std::optional<platform::SyntheticCampaignScoring> scoring;
    if (!fixture.bare) scoring = platform::SyntheticCampaignScoring{
        (fixture.faction == "REBEL" ? "TEXT_REBEL_TITLE" : "TEXT_EMPIRE_TITLE") + std::string("7"),
        {0.82, std::nullopt, 0.0}};
    auto services = platform::campaign_end_services(audio_catalog, std::move(scoring));
    live.services = std::move(services);
    return true;

}

bool CaptureHudFixture::run_score_ledger(GalacticMode::State& state) {
    auto& options = state.options;
    auto& failure = state.failure;
    auto& galaxy = state.galaxy;
    auto& galaxy_setup = state.galaxy_setup;
    auto& game = state.game;
    auto& neutral_owner = state.neutral_owner;

    if (!score_ledger_script) return true;
    const auto local = galaxy->local.player;
    sim::galactic::PlayerId other = 0;
    for (const auto& player : galaxy_setup.players)
        if (player.player_id != local && player.player_id != neutral_owner) { other = player.player_id; break; }
    sim::galactic::PlanetId home = 0, target = 0;
    for (const auto& planet : galaxy_setup.planets) {
        if (!home && planet.owner == local) home = planet.id;
        if (!target && planet.owner == other) target = planet.id;
    }
    if (!home || !target) { failure = "ledger score fixture needs a local and an opposing planet"; return false; }
    const auto publish = [&](sim::galactic::CommandPayload payload) {
        const auto tick = game->frame().session_tick + 1;
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, std::move(payload), {}});
        game->advance_to(tick);
        return game->wait_for(tick, std::chrono::seconds(10));
    };
    if (options.paused && !publish(sim::galactic::ResumeGalaxy{})) {
        failure = "ledger score fixture could not resume";
        return false;
    }
    if (!publish(sim::galactic::ProductionCommand{home, 1000, sim::galactic::ProductionAction::buy,
        sim::galactic::ProductionDomain::space, 0, {}})) {
        failure = "ledger score fixture build was not admitted";
        return false;
    }
    // The synthetic offer builds in ten seconds (300 frames); wait past its completion.
    const auto done = game->frame().session_tick + 330;
    game->advance_to(done);
    if (!game->wait_for(done, std::chrono::seconds(30))) {
        failure = "ledger score fixture production did not finish";
        return false;
    }
    for (const auto& [planet, owner] : {std::pair{target, local}, std::pair{home, other}, std::pair{target, other}}) {
        if (!publish(sim::galactic::DebugSetPlanetOwner{planet, owner})) {
            failure = "ledger score fixture ownership change was not published";
            return false;
        }
    }
    if (options.paused && !publish(sim::galactic::PauseGalaxy{})) {
        failure = "ledger score fixture could not pause";
        return false;
    }
    return true;
}

std::optional<presentation::ui::PixelRect> CaptureHudFixture::ground_stack_rect(
    GalacticMode::State& state, std::string_view name, std::size_t slot,
    const presentation::ui::ShellPlacement& placement) const {
    if (!enabled(Feature::ground_placement) || slot >= 3 || !name.ends_with("_fleet")) return {};
    // Explicit synthetic input qualification; live aggregate parking stays GHUDL-09.
    constexpr std::array<std::string_view, 3> cells{"q1_0001", "q2_0001", "q3_0001"};
    if (const auto* cell = state.fleet_anchors.find(cells[slot])) {
        auto rect = presentation::ui::shell_to_screen(cell->rect, placement);
        rect.y -= rect.height * 1.25;
        return rect;
    }
    return {};
}

std::shared_ptr<GalacticHudFixture> make_capture_hud_fixture(const GalacticMode::Options& options) {
    if (options.hud_fixture.empty()) return {};
    return std::make_shared<CaptureHudFixture>(options);
}
} // namespace eawr::presentation::godot_backend
