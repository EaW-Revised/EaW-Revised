#include "live_battle_bounty.hpp"
#include "eawr/battle/start.hpp"
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
        battle_tactical_types.emplace(battle::type_id(name), name);
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
            || id == "IDD_CAMPAIGN_SCORE_DIALOG" ? model::Placement::full_screen : model::Placement::centre, false,
            [this](std::string_view cue) { if (audio) audio->ui_sound(cue); });
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
            if (options.hud_provider) {
                if (const auto result = options.hud_provider->choose_battle(*this, id, choice)) return *result;
            }
            return battle_choice_sink && battle_choice_sink(id, choice);
        });
    if (options.hud_provider && !options.hud_provider->battle_dialog(*this)) return false;
    return true;
}
void GalacticMode::State::service_battle_dialog(double delta) {
    static_cast<void>(delta);
    if (!battle_dialog) return;
    if (options.hud_provider) options.hud_provider->publish_battle(*this);
    battle_dialog->service_layout();
    if (audio) {
        const auto& view = battle_dialog->view();
        audio->battle_summary(view.conflict, view.phase == galactic_view::BattleDialogPhase::summary, view.won);
    }
    if (battle_dialog->visible()) {
        cancel_fleet_drag();
        // WGUI-171: the pending HUD shell leaves keyboard camera input live.
        // Progress and summary retain their modal input barrier.
        if (battle_dialog->view().phase != galactic_view::BattleDialogPhase::choice) held.clear();
        orbiting = false; scrolling = false;
    }
}
}
