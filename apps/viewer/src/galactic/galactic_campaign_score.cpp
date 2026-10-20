#include "galactic_mode_internal.hpp"
#include "eawr/core/diagnostic.hpp"
#include "eawr/sim/galactic/victory.hpp"

using namespace godot;
namespace eawr::presentation::godot_backend {
bool GalacticMode::State::build_campaign_score() {
    if (!hud || !battle_dialog_builder || !battle_text) return true;
    CampaignScoreDialog::Style style;
    style.dialog = battle_dialog_builder;
    style.text = battle_text;
    style.texture = [this](std::string_view name) { return command_texture(name); };
    const auto fixed = options.hud_provider ? options.hud_provider->score_space() : std::nullopt;
    campaign_score = std::make_unique<CampaignScoreDialog>(*hud, std::move(style),
        [this, fixed] { return fixed ? *fixed : (presentation_random() & 1U) == 0; },
        [this](platform::CampaignScoreDestination destination) { score_destination = destination; });
    return true;
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
    if (campaign_score->visible()) { held.clear(); orbiting = false; scrolling = false; }
}

}
