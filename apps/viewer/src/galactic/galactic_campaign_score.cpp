#include "galactic_mode_internal.hpp"
#include "eawr/campaign/campaign_switch.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/data/ui/movie.hpp"
#include "eawr/data/galactic/galactic_data.hpp"
#include <godot_cpp/variant/utility_functions.hpp>
#include "eawr/sim/galactic/victory.hpp"

using namespace godot;
namespace eawr::presentation::godot_backend {
bool GalacticMode::State::build_campaign_score() {
    if (!hud || !battle_dialog_builder || !battle_text) return true;
    CampaignScoreDialog::Style style;
    style.dialog = battle_dialog_builder;
    style.text = battle_text;
    style.texture = [this](std::string_view name) { return command_texture(name); };
    // WGP-107..110: the end chain's requests resolve through the effective movies.xml; a Has_Credits declaration rolls
    // the credits under the score, keys left to the score's movie skip (WGP-109).
    style.movie = [this](std::string_view name) -> std::optional<viewer::galactic::CampaignMovieDeclaration> {
        if (!filesystem || name.empty()) return std::nullopt;
        const auto declared = data::ui::resolve_movie_definition(*filesystem, name);
        if (!declared) return std::nullopt;
        return viewer::galactic::CampaignMovieDeclaration{declared.value().has_credits, declared.value().cannot_skip};
    };
    style.open_credits = [this]() -> EawrCreditsScreen* {
        if (!filesystem || !hud_fonts || !hud) return nullptr;
        CreditsContext context;
        context.filesystem = filesystem.get();
        context.fonts = hud_fonts.get();
        context.viewport_size = hud->get_viewport_rect().size;
        context.layer = EawrCreditsScreen::layer_index;
        context.input_skips = false;
        auto* screen = memnew(EawrCreditsScreen);
        if (!screen->show(*hud, context)) { memdelete(screen); return nullptr; }
        return screen;
    };
    style.stop_music_and_speech = [this] { if (audio) audio->campaign_end_stop(); };
    const auto fixed = options.hud_provider ? options.hud_provider->score_space() : std::nullopt;
    campaign_score = std::make_unique<CampaignScoreDialog>(*hud, std::move(style),
        [this, fixed] { return fixed ? *fixed : (presentation_random() & 1U) == 0; },
        [this](platform::CampaignScoreDestination destination) { score_destination = destination; });
    return true;
}

// LOAD_CAMPAIGN (WST-95): a published switch request is resolved once, against the campaign data, before anything is torn
// down. A target the data refuses (unknown campaign, a side the entry does not seat) is warned about and settled here, and
// the campaign goes on; an accepted one is left for the host (GalacticMode::campaign_switch()).
void GalacticMode::State::service_campaign_switch() {
    if (!game || !galaxy || campaign_switch) return;
    const auto frame = game->frame(sim::SeatId{galaxy->local.seat});
    if (!frame.campaign_switch || frame.campaign_switch->tick == switch_handled_tick) return;
    switch_handled_tick = frame.campaign_switch->tick;
    std::string refusal;
    if (!filesystem || !marker_catalog) refusal = "the campaign data is not mounted";
    else if (auto loaded = data::galactic::load_galactic_data(*filesystem, *marker_catalog); !loaded) refusal = core::format_diagnostic(loaded.error());
    else if (auto target = campaign::resolve_campaign_switch(loaded.value(), *frame.campaign_switch); !target) refusal = target.error().message;
    else { campaign_switch = std::move(target).value(); return; }
    UtilityFunctions::printerr(String::utf8(campaign::campaign_switch_warning(refusal).c_str()));
    game->settle_campaign_switch();
}

bool GalacticMode::State::score_dialog_open() const {
    return (battle_dialog && battle_dialog->visible()) || retreat_dialog_open
        || (save_controls && save_controls->visible());
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
    campaign_score->service_movies();
    if (campaign_score->visible()) { held.clear(); orbiting = false; scrolling = false; }
}

}
