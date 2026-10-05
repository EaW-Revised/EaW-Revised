#include "tactical_hud_internal.hpp"

namespace eawr::presentation::godot_backend {
using tactical_hud_detail::text;
using tactical_hud_detail::rect2;

using tactical_hud_detail::colour;

bool TacticalHud::build(const vfs::Vfs& filesystem, const data::Catalog* objects,
                        const std::optional<std::string>& context_name, Node& parent) {
    State& state = *state_;
    auto command_bar = data::ui::load_command_bar(filesystem);
    if (!command_bar) {
        state.failure = core::format_diagnostic(command_bar.error());
        return false;
    }
    const data::ui::CommandBarCatalog& catalog = command_bar.value().catalog;
    state.shell_model = model::tactical_shell_model(catalog);
    auto anchors = data::ui::load_shell_anchors(filesystem, state.shell_model);
    if (!anchors) {
        state.failure = core::format_diagnostic(anchors.error());
        return false;
    }
    for (const auto& diagnostic : anchors.value().diagnostics) state.diagnostics.push_back(diagnostic);
    state.shell = model::hud_shell(anchors.value().shell, catalog, state.options.faction);
    for (const auto& diagnostic : state.shell.diagnostics) state.diagnostics.push_back(diagnostic);

    EawrTacticalHud::Setup setup;
    setup.rules = state.options.rules;
    setup.view = model::hud_view_model(anchors.value().shell, catalog, model::alt_variant(state.options.faction),
                                       model::vfs_shell_masks(filesystem));
    for (const auto& diagnostic : setup.view.diagnostics) state.diagnostics.push_back(diagnostic);
    state.faceplates = setup.view.faceplates.size();

    // Shell art: texture files under Data/Art/Textures, decoded once per name.
    const model::StandaloneTextures standalone = model::vfs_standalone_textures(filesystem);
    std::map<std::string, Ref<Texture2D>> loaded;
    for (const model::HudShellMesh& mesh : state.shell.meshes) {
        auto [found, inserted] = loaded.try_emplace(mesh.texture);
        std::string origin = "loaded";
        if (inserted) {
            std::string why;
            if (const auto path = standalone(mesh.texture)) {
                if (auto texture = assets::load_texture(filesystem, *path)) {
                    const Ref<Image> image = texture_image(texture.value(), why);
                    if (image.is_valid()) found->second = ImageTexture::create_from_image(image);
                } else {
                    why = core::format_diagnostic(texture.error());
                }
            } else {
                why = "no texture file";
            }
            if (found->second.is_null()) {
                origin = "missing";
                state.warn("EAWR-UI-0322", mesh.name + ": " + mesh.texture + " cannot be drawn: " + why, mesh.texture);
            }
        } else if (found->second.is_null()) {
            origin = "missing";
        }
        state.mesh_textures.push_back(mesh.name + ": " + mesh.texture + " (" + origin + ")");
        setup.meshes.push_back({mesh.blend, found->second, mesh.triangles});
    }

    // The options button's state textures: the command bar's atlas, then files.
    state.atlas_path = model::command_bar_mega_texture(catalog);
    if (auto atlas = assets::load_mega_texture_atlas(filesystem, state.atlas_path)) {
        state.atlas.emplace(std::move(atlas.value()));
        state.textures = std::make_unique<UiTextures>(*state.atlas, filesystem);
        if (!state.textures->page_failure().empty()) {
            state.warn("EAWR-UI-0322", "atlas page: " + state.textures->page_failure(), state.atlas_path);
        }
    } else {
        state.warn("EAWR-UI-0322", core::format_diagnostic(atlas.error()), state.atlas_path);
    }
    // A button texture at texel size, "missing" when it cannot be drawn.
    const auto button_texture = [&](const std::string& button, const std::string& name, std::string& origin) {
        const model::ThemeTexture slot =
            model::resolve_ui_texture(name, state.atlas ? &state.atlas->directory : nullptr, standalone);
        Ref<Texture2D> result = state.textures ? state.textures->texture(slot, 1.0, 1.0) : Ref<Texture2D>();
        origin = result.is_valid() ? std::string(model::to_string(slot.origin)) : "missing";
        if (result.is_null()) state.warn("EAWR-UI-0322", button + ": " + name + " cannot be drawn", name);
        return result;
    };
    // Button art keeps its texture's size around the bone (button_quad).
    const auto quad = [](const model::HudShellButton& button, const Ref<Texture2D>& texture) {
        const Vector2 size = texture.is_valid() ? texture->get_size() : Vector2(button.rect.width, button.rect.height);
        return model::button_quad(button, size.x, size.y);
    };
    if (state.shell.options) {
        const model::HudShellButton& look = state.shell.options.value();
        state.options_button = memnew(EawrHudButton);
        state.options_button->set_name("b_option_t");
        const auto texture = [&](const std::string& state_name, const std::string& name) -> Ref<Texture2D> {
            if (name.empty()) return {};
            std::string origin;
            Ref<Texture2D> result = button_texture("b_option_t " + state_name, name, origin);
            state.button_textures.push_back(state_name + ": " + name + " (" + origin + ")");
            return result;
        };
        const Ref<Texture2D> normal = texture("normal", look.normal);
        state.options_button->set_texture_normal(normal);
        state.options_button->set_texture_hover(texture("mouse_over", look.mouse_over));
        state.options_button->set_texture_pressed(texture("pressed", look.pressed));
        state.options_button->set_texture_disabled(texture("disabled", look.disabled));
        setup.options_quad = quad(look, normal);
        setup.options_hit = look.rect;
    }
    for (const model::HudShellButton& button : state.shell.panel_buttons) {
        std::string origin = "none";
        Ref<Texture2D> texture;
        if (!button.normal.empty()) texture = button_texture(button.name, button.normal, origin);
        const data::ui::ReferenceRect rect = quad(button, texture);
        state.panel.push_back({button.name, button.normal, origin, rect});
        // #459: pause and fast forward are toggle buttons with their four state textures
        // (TM-05, TM-06); help and holocron stay inert art.
        const bool pause = button.name == "b_play_pause_t";
        if (!pause && button.name != "b_fast_forward_t") {
            setup.panel.push_back({texture, rect});
            continue;
        }
        auto* control = memnew(EawrHudButton);
        control->set_name(String(button.name.c_str()));
        control->set_toggle_mode(true);
        // TM-06: fast forward acts on the press, pause on the release.
        if (!pause) control->set_action_mode(BaseButton::ACTION_MODE_BUTTON_PRESS);
        const auto state_texture = [&](const std::string& state_name, const std::string& name) -> Ref<Texture2D> {
            if (name.empty()) return {};
            std::string texture_origin;
            Ref<Texture2D> result = button_texture(button.name + " " + state_name, name, texture_origin);
            state.time_textures.push_back(button.name + " " + state_name + ": " + name + " (" + texture_origin + ")");
            return result;
        };
        control->set_texture_normal(texture);
        control->set_texture_hover(state_texture("mouse_over", button.mouse_over));
        control->set_texture_pressed(state_texture("pressed", button.pressed));
        control->set_texture_disabled(state_texture("disabled", button.disabled));
        if (pause) {
            std::string origin;
            control->set_flash_texture(button_texture(button.name + " flash",
                button.flash.empty() ? button.pressed : button.flash, origin));
        }
        control->set_action([&state, pause] {
            const auto& handler = pause ? state.time_handlers.pause : state.time_handlers.fast_forward;
            if (handler) handler();
        });
        (pause ? state.pause_button : state.fast_forward_button) = control;
        setup.time_buttons.push_back({control, rect, button.rect});
    }

    // The planet name: its component font through UI-F3, the text through the map.
    std::optional<data::ui::TextDatabase>& text_database = state.text_database;
    if (auto loaded_text = data::ui::load_language_text_database(filesystem, state.options.language)) {
        text_database.emplace(std::move(loaded_text.value()));
    } else {
        state.warn("EAWR-UI-0321", core::format_diagnostic(loaded_text.error()));
    }
    state.objects = objects;
    state.standalone = standalone;
    state.planet = model::planet_name(context_name, objects, text_database ? &*text_database : nullptr);
    for (const auto& diagnostic : state.planet.diagnostics) state.diagnostics.push_back(diagnostic);
    if (state.shell.planet_name) {
        state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        state.planet_face = state.fonts->resolve(
            {state.shell.planet_name->face, state.shell.planet_name->point_size}, state.options.language);
        if (state.planet_face.substituted || state.planet_face.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", state.shell.planet_name->face + " is unavailable (font cache "
                           + state.fonts->cache().directory + "); the planet name uses "
                           + (state.planet_face.face.empty() ? std::string("the engine font") : state.planet_face.face));
        }
        setup.planet = state.shell.planet_name;
        setup.planet->point_size = state.planet_face.point_size;
        setup.planet_font = state.fonts->font(state.planet_face);
        setup.planet_cell = [fonts = state.fonts, face = state.planet_face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        setup.planet_text = text(state.planet.text);
    }

    // Canvas layer -> hit mask (UI-I2) -> drawing surface -> button.
    auto* layer = memnew(CanvasLayer);
    layer->set_name("EawrTacticalHudLayer");
    state.mask = memnew(EawrUiHitMask);
    state.mask->set_name("EawrTacticalHudMask");
    state.hud = memnew(EawrTacticalHud);
    state.hud->set_name("EawrTacticalHud");
    EawrTacticalHud* hud = state.hud;
    state.mask->set_hit_test([hud](const double x, const double y) { return hud->hit(x, y); });
    state.mask->add_child(state.hud);
    layer->add_child(state.mask);
    parent.add_child(layer);
    state.hud->setup(std::move(setup), state.options_button);

    // #425: the unit cards over the shell, in the slots' own face.
    if (!state.shell.card_slots.empty()) {
        if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        const model::HudCardSlot& first = state.shell.card_slots.front();
        state.card_face = state.fonts->resolve({first.face, first.point_size}, state.options.language);
        if (state.card_face.substituted || state.card_face.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", first.face + " is unavailable (font cache " + state.fonts->cache().directory
                           + "); the unit card counts use "
                           + (state.card_face.face.empty() ? std::string("the engine font") : state.card_face.face));
        }
        EawrUnitCards::Setup cards;
        cards.slots = state.shell.card_slots;
        cards.borders = state.shell.card_borders;
        cards.texture = [&state](const std::string& name) { return state.command_texture(name, "unit card"); };
        cards.icon = [&state](const std::string& type) { return state.card_type(type).first; };
        cards.name = [&state](const std::string& type) { return state.card_type(type).second; };
        cards.description = [&state](const std::string& type) {
            return text(model::unit_card_looks(type, state.objects,
                state.text_database ? &*state.text_database : nullptr).description);
        };
        if (const auto* back = catalog.find("encyclopedia_back")) cards.tooltip_size = back->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
        if (const auto* body = catalog.find("encyclopedia_text")) {
            cards.tooltip_point_size = body->integer(data::ui::Field::font_point_size).value_or(0);
            if (const auto size = body->vec2(data::ui::Field::size)) cards.tooltip_size.y = size->y;
        }
        cards.font = state.fonts->font(state.card_face);
        cards.cell = [fonts = state.fonts, face = state.card_face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        cards.point_size = state.card_face.point_size;
        // GameConstants.xml Encyclopedia_Delay: 750 ms before a hovered card opens its encyclopedia.
        cards.hover_delay_seconds = 0.75;
        cards.placement = [hud] { return hud->placement(); };
        cards.space = [hud] { return hud->space(); };
        state.cards = memnew(EawrUnitCards);
        state.cards->set_name("EawrUnitCards");
        state.hud->add_child(state.cards);
        state.cards->setup(std::move(cards));
    }
    // #454: the ability buttons and the cards' ability marks, over the cards.
    if (!state.shell.ability_buttons.empty()) {
        EawrAbilityButtons::Setup abilities;
        abilities.buttons = state.shell.ability_buttons;
        abilities.slots = state.shell.card_slots;
        abilities.texture = [&state](const std::string& name) { return state.command_texture(name, "ability button"); };
        abilities.placement = [hud] { return hud->placement(); };
        state.abilities = memnew(EawrAbilityButtons);
        state.abilities->set_name("EawrAbilityButtons");
        state.hud->add_child(state.abilities);
        state.abilities->setup(std::move(abilities));
    }
    // #530: the build queue, the credits and the reinforcement pane (space-purchasing PU-63 to
    // PU-67), in the card slots' face.
    if (!state.shell.queue_slots.empty() || state.shell.credits || state.shell.reinforcement) {
        EawrProductionPanel::Setup production;
        production.queue = state.shell.queue_slots;
        production.credits = state.shell.credits;
        production.reinforce = state.shell.reinforcement;
        if (auto pane_anchors = data::ui::load_shell_anchors(filesystem, model::reinforce_pane_model(catalog))) {
            for (const auto& diagnostic : pane_anchors.value().diagnostics) state.diagnostics.push_back(diagnostic);
            auto pane = model::reinforce_pane(pane_anchors.value().shell, catalog, text_database ? &*text_database : nullptr);
            for (const auto& diagnostic : pane.diagnostics) state.diagnostics.push_back(diagnostic);
            production.pane = std::move(pane);
        } else {
            state.diagnostics.push_back(pane_anchors.error());
        }
        production.texture = [&state](const std::string& name) { return state.command_texture(name, "production"); };
        production.optional_texture = [&state](const std::string& name) { return state.command_texture(name, "production", true); };
        production.icon = [&state](const std::string& type) { return state.card_type(type).first; };
        if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
        const model::ResolvedFont face = state.fonts->resolve({"EmpireAtWar-Medium", 6}, state.options.language);
        production.font = state.fonts->font(face);
        if (production.pane && production.pane->close_text) {
            const auto& label = production.pane->close_text->text;
            const auto close_face = state.fonts->resolve({label.face, label.point_size}, state.options.language);
            production.close_font = state.fonts->font(close_face);
            production.close_cell = [fonts = state.fonts, close_face](const std::int32_t height) {
                return model::gdi_text_cell(fonts->cache(), close_face, height);
            };
        }
        production.cell = [fonts = state.fonts, face](const std::int32_t height) {
            return model::gdi_text_cell(fonts->cache(), face, height);
        };
        production.placement = [hud] { return hud->placement(); };
        production.space = [hud] { return hud->space(); };
        state.production = memnew(EawrProductionPanel);
        state.production->set_name("EawrProductionPanel");
        state.hud->add_child(state.production);
        state.production->setup(std::move(production));
    }
    // #455: the minimap inside the radar mesh; its icons are command bar textures (MM-06).
    if (state.shell.minimap) {
        state.minimap_settings = model::minimap_settings(filesystem);
        for (const auto& diagnostic : state.minimap_settings.diagnostics) state.diagnostics.push_back(diagnostic);
        EawrMinimap::Setup minimap;
        minimap.rect = *state.shell.minimap;
        minimap.backdrop = state.minimap_settings.backdrop;
        minimap.background = state.minimap_settings.background;
        minimap.texture = [&state](const std::string& name) { return state.command_texture(name, "minimap"); };
        minimap.placement = [hud] { return hud->placement(); };
        state.minimap = memnew(EawrMinimap);
        state.minimap->set_name("EawrMinimap");
        state.hud->add_child(state.minimap);
        state.minimap->setup(std::move(minimap));
        state.minimap->set_look([&state](const model::MinimapPoint point) {
            const auto world = model::minimap_world(state.minimap_extents, point);
            if (state.minimap_look) state.minimap_look(world[0], world[1]);
        });
        state.minimap->set_move([&state](const model::MinimapPoint point) {
            const auto world = model::minimap_world(state.minimap_extents, point);
            if (state.minimap_move) state.minimap_move(world[0], world[1]);
        });
    }
    // #453, #459: the battle overlay above the HUD. Texts from the text DB, looks from
    // GameConstants.xml (BE-03, TM-09) and the pause banner's component fonts.
    state.message_looks = model::battle_message_looks(filesystem);
    for (const auto& diagnostic : state.message_looks.diagnostics) state.diagnostics.push_back(diagnostic);
    if (!state.fonts) state.fonts = std::make_shared<FontProvider>(std::move(state.options.font_cache));
    const auto overlay_font = [&](const std::string& face, const std::int32_t points, const std::string& use) {
        const model::ResolvedFont resolved = state.fonts->resolve({face, points}, state.options.language);
        if (resolved.substituted || resolved.source == model::FaceSource::engine_default) {
            state.warn("EAWR-UI-0321", face + " is unavailable (font cache " + state.fonts->cache().directory + "); the "
                           + use + " falls back to " + (resolved.face.empty() ? std::string("the engine font") : resolved.face));
        }
        return EawrBattleOverlay::Font{state.fonts->font(resolved), resolved.point_size};
    };
    const auto game_text = [&](const std::string_view key) {
        const data::ui::TextEntry* entry = text_database ? text_database->find(key) : nullptr;
        if (entry == nullptr) {
            state.warn("EAWR-UI-0321", std::string(key) + " is not in the text DB; the key is shown");
            return text(key);
        }
        return text(data::ui::to_utf8(entry->value));
    };
    // TM-09: the pause text and its button take the pause shell's text components' faces
    // (text_attack, attack_button: EmpireAtWar-Medium 7 in FoC).
    const auto component_font = [&](const std::string_view name) -> std::pair<std::string, std::int32_t> {
        const data::ui::CommandBarComponent* component = catalog.find(name);
        if (component == nullptr) return {"EmpireAtWar-Medium", 7};
        const auto faces = component->list(data::ui::Field::font_name);
        return {faces.empty() ? std::string("EmpireAtWar-Medium") : faces.front(),
                component->integer(data::ui::Field::font_point_size).value_or(7)};
    };
    for (std::size_t index = 0; index < state.world_group_fonts.size(); ++index) {
        const auto [face, points] = component_font(index == 0 ? "st_grab_bar" : "st_control_group");
        state.world_group_fonts[index] = state.fonts->font(state.fonts->resolve({face, points}, state.options.language));
    }
    const data::ui::CommandBarComponent* resume_component = catalog.find("attack_button");
    const data::ui::Rgba8 resume_colour = resume_component != nullptr
        ? resume_component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 64, 64, 255})
        : data::ui::Rgba8{255, 64, 64, 255};
    EawrBattleOverlay::Style overlay;
    overlay.message = overlay_font(state.message_looks.font, state.message_looks.point_size, "win/lose message");
    overlay.win = colour(state.message_looks.win);
    overlay.lose = colour(state.message_looks.lose);
    const auto [banner_face, banner_points] = component_font("text_attack");
    overlay.banner = overlay_font(banner_face, banner_points, "pause banner");
    overlay.paused = colour(state.message_looks.pending);
    // BEP-03/TP-07: the fallback and extra project captions keep their 12-point face.
    // The native Resume caption below uses the component's authored point size.
    const auto [button_face, button_points] = component_font("attack_button");
    overlay.button = overlay_font(button_face, 12, "battle buttons");
    overlay.button_colour = colour(resume_colour);
    overlay.native_button = overlay_font(button_face, button_points, "native Resume button");
    overlay.button_emboss = resume_component && resume_component->flag(data::ui::Field::text_emboss);
    overlay.button_outline = resume_component && resume_component->flag(data::ui::Field::text_outline);
    if (auto pause = model::pause_shell(filesystem, catalog)) {
        for (const auto& anchor : pause.value().anchors.anchors()) {
            if (anchor.name == "attack_button" || anchor.bone == "attack_button") overlay.pause_button = anchor.rect;
            if (anchor.name == "text_attack" || anchor.bone == "text_attack") overlay.pause_text = anchor.rect;
            if (anchor.name == "attack_frame" && pause.value().text_origin) {
                const auto origin = *pause.value().text_origin;
                overlay.pause_text = data::ui::ReferenceRect{origin.x - anchor.rect.width / 2.0F,
                    origin.y, anchor.rect.width, 0.0F};
            }
        }
        for (const auto& mesh : pause.value().meshes) {
            overlay.pause_meshes.push_back({mesh.blend, state.command_texture(mesh.texture, "pause shell"), mesh.triangles});
        }
        overlay.pause_rollover = state.command_texture(resume_component
            ? std::string(resume_component->text(data::ui::Field::mouse_over_texture_name)) : std::string(), "Resume rollover");
        if (!overlay.pause_text || !overlay.pause_button) state.warn("EAWR-UI-0320", "pause shell lacks its text/button anchors");
    } else {
        state.diagnostics.push_back(pause.error());
    }
    overlay.win_text = game_text(model::battle_message_key(model::BattleResult::victory));
    overlay.lose_text = game_text(model::battle_message_key(model::BattleResult::defeat));
    overlay.victory_title = game_text("TEXT_WIN_BATTLE");
    overlay.defeat_title = game_text("TEXT_LOST_BATTLE");
    overlay.paused_text = game_text(model::pause_text_key);
    overlay.resume_text = game_text(model::pause_resume_key);
    overlay.quit_text = game_text(model::battle_end_quit_key);
    overlay.begin_text = game_text("TEXT_BUTTON_BEGIN");
    overlay.loading_title = game_text("TEXT_LOAD_MAP_TITLE");
    overlay.space = [hud] { return hud->space(); };
    overlay.battle_time = game_text("TEXT_BATTLE_TIME");
    overlay.exit_text = game_text("TEXT_BUTTON_CONTINUE");
    overlay.result_icon = [&state](const std::string& type) {
        return state.command_texture(state.card_type(type).first, "battle result", true);
    };
    overlay.result_name = [&state](const std::string& type) { return state.card_type(type).second; };
    // WBF-44/45: the installed dialog owns the geometry, skin and captions.
    if (auto loaded = data::ui::load_dialog_catalog(filesystem)) {
        auto skin = std::make_shared<data::ui::DialogCatalog>(std::move(loaded).value());
        if (auto loaded_page = assets::load_mega_texture_atlas(filesystem,
            "data/art/textures/" + skin->skin.texture_file + ".mtd")) {
            auto page = std::make_shared<assets::MegaTextureAtlas>(std::move(loaded_page).value());
            auto textures = std::make_shared<UiTextures>(*page, filesystem);
            overlay.results_dialog = [&state, skin, page, textures, fonts = state.fonts](const Vector2 size) {
                const auto* dialog = skin->script.find("IDD_BATTLE_END_DIALOG");
                if (dialog == nullptr) {
                    state.warn("EAWR-UI-0321", "IDD_BATTLE_END_DIALOG is absent; results use the fallback panel");
                    return BuiltDialog{};
                }
                model::ThemeSources sources;
                sources.catalog = skin.get();
                sources.atlas = &page->directory;
                sources.standalone = state.standalone;
                sources.fonts = &fonts->cache();
                sources.language = state.options.language;
                const auto& families = fonts->system_families();
                sources.system = [&families](const std::string_view face) {
                    return model::match_system_face(face, families).has_value();
                };
                const auto space = model::reference_space({static_cast<std::uint32_t>(size.x),
                    static_cast<std::uint32_t>(size.y)}, state.options.rules);
                const auto theme = model::build_theme_model(sources, space);
                auto built = build_dialog(*dialog, theme, nullptr, model::Placement::full_screen);
                built.frame->set_theme(build_theme(theme, *textures, *fonts));
                for (auto& gadget : built.gadgets) {
                    if (auto* label = Object::cast_to<EawrUiLabel>(gadget.control)) {
                        // WBF-43/44: results replaces the skin's default title/time
                        // ink with white; the authored font and alignment remain.
                        if (gadget.id == "IDC_TITLE_STATIC" || gadget.id == "IDC_TIME_STATIC") {
                            const String role = gadget.statement == "RTEXT" ? "R_Text" : "L_Text";
                            label->add_theme_color_override(role + String("_top"), Color(1, 1, 1, 1));
                            label->add_theme_color_override(role + String("_bottom"), Color(1, 1, 1, 1));
                        }
                        const auto* entry = state.text_database ? state.text_database->find(gadget.caption) : nullptr;
                        if (entry) label->set_text(text(data::ui::to_utf8(entry->value)));
                        else {
                            label->set_text(text(gadget.caption));
                            if (gadget.caption.starts_with("TEXT_")) state.warn("EAWR-UI-0321", gadget.caption + " is not in the text DB; the key is shown");
                        }
                    }
                }
                return built;
            };
        } else state.warn("EAWR-UI-0322", core::format_diagnostic(loaded_page.error()));
    } else state.warn("EAWR-UI-0321", core::format_diagnostic(loaded.error()));
    state.overlay = memnew(EawrBattleOverlay);
    state.overlay->set_name("EawrBattleOverlay");
    layer->add_child(state.overlay);
    state.overlay->setup(std::move(overlay),
        [&state] { if (state.time_handlers.resume) state.time_handlers.resume(); },
        [&state] { if (state.time_handlers.quit) state.time_handlers.quit(); });
    if (state.options.probe) state.hud->set_probe([&state] { state.run_probe(); });
    return true;
}


void register_tactical_hud_classes() {
    GDREGISTER_CLASS(EawrTacticalHud);
    GDREGISTER_CLASS(EawrHudButton);
    GDREGISTER_CLASS(EawrUnitCards);
    GDREGISTER_CLASS(EawrProductionPanel);
    GDREGISTER_CLASS(EawrMinimap);
    GDREGISTER_CLASS(EawrAbilityButtons);
    GDREGISTER_CLASS(EawrBattleOverlay);
    GDREGISTER_CLASS(EawrOverlayButton);
    GDREGISTER_CLASS(EawrPerfOverlay);
}

} // namespace eawr::presentation::godot_backend
