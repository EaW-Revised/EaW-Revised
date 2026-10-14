#include "live_battle_bounty.hpp"
#include "galactic_mode_internal.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/data/retail_values.hpp"
#include "ui/theme_builder.hpp"
#include <godot_cpp/classes/theme.hpp>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;
bool GalacticMode::State::build_battle_dialog() {
    ++battle_metadata_generation;
    if (!filesystem || !hud || !hud_fonts) return true;
    auto loaded = data::ui::load_dialog_catalog(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    auto catalog = std::make_shared<data::ui::DialogCatalog>(std::move(loaded).value());
    auto loaded_atlas = assets::load_mega_texture_atlas(*filesystem,
        "data/art/textures/" + catalog->skin.texture_file + ".mtd");
    if (!loaded_atlas) { failure = core::format_diagnostic(loaded_atlas.error()); return false; }
    auto atlas = std::make_shared<assets::MegaTextureAtlas>(std::move(loaded_atlas).value());
    auto textures = std::make_shared<UiTextures>(*atlas, *filesystem);
    auto loaded_text = data::ui::load_language_text_database(*filesystem, "english");
    if (!loaded_text) { failure = core::format_diagnostic(loaded_text.error()); return false; }
    auto database = std::make_shared<data::ui::TextDatabase>(std::move(loaded_text).value());
    BattleDialog::Style style;
    style.text = [database](std::string_view key) {
        const auto* row = database->find(key);
        return row ? data::ui::to_utf8(row->value) : std::string(key);
    };
    battle_text = style.text;
    // WGP-34: the summary picture belongs to the local faction.
    if (audio_catalog && content) for (const auto& [player, faction] : content->factions) {
        auto resolved = audio_catalog->resolve(faction.faction, data::Category::faction);
        if (!resolved) continue;
        auto& pictures = battle_space_pictures[player];
        for (std::size_t index = 0; index < pictures.size(); ++index)
            if (const auto* value = resolved.value().value(index == 0 ? "Space_Win_Image" : "Space_Lose_Image"))
                pictures[index] = value->value.raw_text;
    }
    // WBF-45/46: summary filtering and grouping read original type metadata.
    // Catalog resolution and the validated numeric reader avoid a second
    // tactical model/content load solely to draw loss rows.
    if (audio_catalog) for (const auto& [name, art] : production_art) {
        auto resolved = audio_catalog->resolve(name, data::Category::game_object);
        if (!resolved) continue;
        galactic_view::CampaignResultType info;
        // The display name in `art.text` is not a key; resolve from the Text_ID.
        info.name = art.text_id.empty() ? name : battle_text(art.text_id);
        if (const auto* text = database->find(art.text_id)) info.lexical_name = text->value;
        else info.lexical_name = galactic_view::campaign_name_units(info.name);
        if (const auto* value = resolved.value().value("Score_Cost_Credits")) {
            auto parsed = data::fixed_value(value->value);
            if (!parsed) { failure = core::format_diagnostic(parsed.error()); return false; }
            info.score_cost = static_cast<double>(parsed.value().raw()) / sim::math::Fixed::scale;
        }
        info.scored = info.score_cost >= 1;
        if (const auto* value = resolved.value().value("AI_Combat_Power")) {
            auto parsed = data::fixed_value(value->value);
            if (!parsed) { failure = core::format_diagnostic(parsed.error()); return false; }
            info.combat_power = static_cast<double>(parsed.value().raw()) / sim::math::Fixed::scale;
        }
        if (const auto* value = resolved.value().value("Is_Named_Hero"))
            info.hero = data::retail::boolean(value->value.raw_text).value_or(false);
        battle_result_types.emplace(name, std::move(info));
        battle_tactical_types.emplace(skirmish::type_id(name), name);
    }
    style.texture = [this](std::string_view type) {
        const auto art = production_art.find(std::string(type));
        return command_texture(art == production_art.end() ? std::string(type) : art->second.icon);
    };
    style.dialog = [this, catalog, atlas, textures, database, text = style.text](std::string_view id, Vector2 size) {
        model::ThemeSources sources;
        sources.catalog = catalog.get(); sources.atlas = &atlas->directory;
        sources.standalone = model::vfs_standalone_textures(*filesystem);
        sources.fonts = &hud_fonts->cache(); sources.language = "english";
        const auto& families = hud_fonts->system_families();
        sources.system = [&families](std::string_view face) { return model::match_system_face(face, families).has_value(); };
        const auto space = model::reference_space({static_cast<std::uint32_t>(size.x), static_cast<std::uint32_t>(size.y)});
        const auto theme = model::build_theme_model(sources, space);
        auto native_theme = build_theme(theme, *textures, *hud_fonts);

        const auto* definition = catalog->script.find(id);
        if (!definition) return BuiltDialog{};
        data::ui::TextLookup lookup(*database);
        // WGP-37: the campaign score is a full-screen dialog like the battle summary.
        auto built = build_dialog(*definition, theme, &lookup, id == "IDD_BATTLE_END_DIALOG"
            || id == "IDD_CAMPAIGN_SCORE_DIALOG" ? model::Placement::full_screen : model::Placement::centre);
        built.frame->set_theme(native_theme);
        for (auto& gadget : built.gadgets) if (gadget.control &&
            (gadget.id == "IDC_TITLE_STATIC" || gadget.id == "IDC_TIME_STATIC")) {
            const String role = gadget.statement == "RTEXT" ? "R_Text" : "L_Text";
            gadget.control->add_theme_color_override(role + String("_top"), Color(1, 1, 1, 1));
            gadget.control->add_theme_color_override(role + String("_bottom"), Color(1, 1, 1, 1));
        }
        return built;
    };
    style.pending = [this, texture = style.texture, text = style.text](Vector2 size, bool single) {
        return build_pending_battle_hud(*filesystem, command_catalog, *hud_fonts, command_faction, size, single, texture, text,
            [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
    };
    battle_dialog_builder = style.dialog;
    battle_dialog = std::make_unique<BattleDialog>(*hud, std::move(style),
        [this](std::uint64_t id, galactic_view::BattleChoice choice) {
            if (id != battle_dialog_fixture.conflict) {
                return battle_choice_sink && battle_choice_sink(id, choice);
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
        });
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
        view.sides[0] = {"Empire", "", {0.9F, 0.2F, 0.2F}, {"Imperial Star Destroyer", "TIE Fighter", "TIE Fighter", "TIE Bomber", "TIE Scout"}};
        view.sides[1] = {"Rebel Alliance", "", {0.2F, 0.9F, 0.2F}, {"Mon Calamari Cruiser", "X-wing"}};
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
void GalacticMode::State::service_battle_dialog(double delta) {
    static_cast<void>(delta);
    if (!battle_dialog) return;
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
    battle_dialog->service_layout();
    if (audio) {
        const auto& view = battle_dialog->view();
        audio->battle_summary(view.conflict, view.phase == galactic_view::BattleDialogPhase::summary, view.won);
    }
    if (battle_dialog->visible()) {
        cancel_fleet_drag();
        held.clear(); orbiting = false; scrolling = false;
    }
}
}
