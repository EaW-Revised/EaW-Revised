#include "live_battle_bounty.hpp"
#include "notice_text.hpp"
#include "eawr/battle/start.hpp"
#include "galactic_mode_internal.hpp"
#include "eawr/data/ui/dialog_catalog.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/presentation/ui/theme.hpp"
#include "eawr/data/retail_values.hpp"
#include "ui/theme_builder.hpp"
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace model = presentation::ui;
bool GalacticMode::State::build_battle_dialog() {
    ++battle_metadata_generation;
    if (!filesystem || !hud || !hud_fonts) return true;
    auto loaded = data::ui::load_dialog_catalog(*filesystem);
    if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
    auto catalog = std::make_shared<data::ui::DialogCatalog>(std::move(loaded).value());
    bool invalid_search_layout{};
    planet_search_layout = galactic_view::load_planet_search_layout(*filesystem, invalid_search_layout);
    if (invalid_search_layout)
        godot::UtilityFunctions::push_warning("EAWR-UI-PLANET-SEARCH: invalid dialog layer; retaining the last valid definition");
    if (planet_search_layout) catalog->script.dialogs.push_back(planet_search_layout->dialog);
    auto loaded_atlas = assets::load_mega_texture_atlas(*filesystem,
        "data/art/textures/" + catalog->skin.texture_file + ".mtd");
    auto atlas = std::make_shared<assets::MegaTextureAtlas>();
    if (loaded_atlas) *atlas = std::move(loaded_atlas).value();
    else {
        UtilityFunctions::push_warning(String::utf8(core::format_diagnostic(loaded_atlas.error()).c_str()));
        pending_texture_notes.push_back(galactic_view::unresolved_ui_texture_note(catalog->skin.texture_file));
    }
    auto textures = std::make_shared<UiTextures>(*atlas, *filesystem);
    textures->set_missing_fallback(missing_shell_texture());
    if (loaded_atlas && !textures->page_failure().empty()) {
        UtilityFunctions::push_warning(String::utf8(textures->page_failure().c_str()));
        pending_texture_notes.push_back(galactic_view::unresolved_ui_texture_note(catalog->skin.texture_file));
    }
    auto loaded_text = data::ui::load_language_text_database(*filesystem, "english");
    if (!loaded_text) { failure = core::format_diagnostic(loaded_text.error()); return false; }
    auto database = std::make_shared<data::ui::TextDatabase>(std::move(loaded_text).value());
    BattleDialog::Style style;
    style.text = [database](std::string_view key) {
        data::ui::TextLookup lookup(*database);
        return data::ui::to_utf8(lookup.text(key));
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
        return build_pending_battle_hud(*filesystem, command_catalog, *hud_fonts, command_faction, size, single, texture,
            [this](std::string_view name) { return shell_texture(name); }, text,
            [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
    };
    style.pending_click_sound = [this](bool single, bool begin) {
        const auto* component = command_catalog.find(begin ? (single ? "attack_button_single" : "choice_button_left")
                                                        : "choice_button_right");
        auto cue = component ? component->text(data::ui::Field::click_sfx) : std::string_view{};
        if (cue.empty()) if (const auto* option_component = command_catalog.find("b_option_g"))
            cue = option_component->text(data::ui::Field::click_sfx);
        if (audio && !cue.empty()) audio->ui_sound(cue);
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
        // #7798: and the middle-mouse / right-button drag, which this per-frame clear used to end.
        // Progress and summary retain their modal input barrier.
        if (battle_dialog->view().phase != galactic_view::BattleDialogPhase::choice) {
            held.clear();
            orbiting = false; scrolling = false;
        }
    }
}
void GalacticMode::State::service_battle_advisor() {
    if (!game || !galaxy || !battle_text || options.hud_provider) return;
    const auto outcome = battle_dialog_frame().auto_resolve_outcome;
    if (!galactic_view::advise_outcome(advised_outcome,
            outcome ? std::optional<std::uint64_t>(outcome->result.conflict) : std::nullopt)) return;
    const auto local = galaxy->local.player;
    const auto planet = static_cast<sim::galactic::PlanetId>(outcome->planet);
    // WGUI-120: an uninvolved observer's result line needs known fleet presence at the planet (read from the
    // publication shown when the outcome arrives). The planet's visibility counts even when no fleet survives there
    // (the original asks the planet, not a fleet), so a land battle between two other factions is announced too.
    const bool presence = std::any_of(fleet_intelligence.presence.begin(), fleet_intelligence.presence.end(),
        [&](const auto& row) { return row.planet == planet; })
        || std::binary_search(fleet_intelligence.presence_planets.begin(), fleet_intelligence.presence_planets.end(), planet);
    std::vector<galactic_view::CampaignRecordEntry> record;
    record.reserve(outcome->record.size());
    for (const auto& row : outcome->record) record.push_back({row.original_type, row.owner});
    const auto faction = [this](std::uint32_t owner) { return faction_display_name(owner); };
    const auto type = [this](std::string_view name) -> const galactic_view::CampaignResultType* {
        const auto at = battle_result_types.find(std::string(name));
        return at == battle_result_types.end() ? nullptr : &at->second;
    };
    const auto lines = galactic_view::auto_resolve_notices(record, local, outcome->invader, outcome->defender,
        outcome->result.winner, outcome->land, presence, faction, planet_notice_name(planet),
        [&](std::string_view name) { const auto* info = type(name); return info && info->hero; },
        [&](std::string_view name) { const auto* info = type(name); return info ? info->name : std::string(name); });
    for (const auto& line : lines) {
        const auto format = battle_text(line.key);
        if (format.empty() || format == line.key) continue; // no text database entry: no invented wording
        AdvisorLine entry;
        entry.text = String::utf8(galactic_view::expand_notice_text(format, line.tokens).c_str());
        entry.colour = {255, 24, 39, 255}; // WAR-64 / WGUI-120: ordinary red entries with no explicit sound (the queue plays the message cue)
        approach_text.push_back(std::move(entry));
    }
}
}
