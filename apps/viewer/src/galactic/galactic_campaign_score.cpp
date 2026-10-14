#include "galactic_mode_internal.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/platform/campaign_end_inputs.hpp"
#include "eawr/platform/campaign_scoring.hpp"
#include "eawr/units/galactic_types.hpp"
#include "hud_clock_fixture.hpp"
#include "eawr/sim/galactic/victory.hpp"

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

bool GalacticMode::State::build_campaign_score() {
    if (!hud || !battle_dialog_builder || !battle_text) return true;
    CampaignScoreDialog::Style style;
    style.dialog = battle_dialog_builder;
    style.text = battle_text;
    style.texture = [this](std::string_view name) { return command_texture(name); };
    const auto fixture = options.campaign == galactic_view::synthetic_campaign
        ? parse_score_fixture(options.hud_fixture) : ScoreFixture{};
    const auto fixed = fixture.valid ? fixture.space : std::nullopt;
    campaign_score = std::make_unique<CampaignScoreDialog>(*hud, std::move(style),
        [this, fixed] { return fixed ? *fixed : (presentation_random() & 1U) == 0; },
        [this](platform::CampaignScoreDestination destination) { score_destination = destination; });
    return true;
}

bool GalacticMode::State::score_dialog_open() const {
    return (battle_dialog && battle_dialog->visible()) || retreat_dialog_open
        || (save_controls && save_controls->visible());
}

bool GalacticMode::State::configure_score_fixture(platform::LiveGameSession::Options& live) {
    const auto fixture = parse_score_fixture(options.hud_fixture);
    if (options.campaign != galactic_view::synthetic_campaign) return true;
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

void GalacticMode::State::service_campaign_score() {
    if (!game || !campaign_score || !galaxy) return;
    const sim::SeatId seat{galaxy->local.seat};
    auto frame = game->frame(seat);
    auto end = frame.campaign_end;
    if (!end && !score_consumed && score_failure.empty() && frame.latest && frame.latest->victory()) {
        // A matured result is the only thing worth asking the owner about; the
        // gates are WGS-82's outer-presentation facts (dialog open, movie playing).
        sim::galactic::GalacticVictoryCondition pending;
        if (sim::galactic::pending_victory(*frame.latest->victory(), pending)) {
            auto consumed = game->consume_campaign_result(seat, score_dialog_open(), false);
            if (!consumed) score_failure = core::format_diagnostic(consumed.error());
            else end = std::move(consumed).value();
        }
    }
    if (end) score_consumed = true;
    campaign_score->present(end);
    campaign_score->service_layout();
    if (campaign_score->visible()) { held.clear(); orbiting = false; scrolling = false; }
}

}

namespace eawr::presentation::godot_backend {
// Scripted ledger history of the -ledger score fixtures: one space unit is built on the local
// planet, then an opposing planet is taken and both planets are lost. It is submitted as
// ordinary galaxy input before the matured result is consumed, so the saved ledger holds one
// build, one acquisition and two losses and the authored module computes the numbers.
bool GalacticMode::State::run_score_ledger() {
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
} // namespace eawr::presentation::godot_backend
