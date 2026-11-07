#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <ostream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
bool option_name(std::string_view name, std::string_view prefix) {
    return name.starts_with(prefix) && name.size() == prefix.size() + 4
        && std::all_of(name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
            [](char value) { return value >= '0' && value <= '9'; });
}
std::string two(std::size_t index) { return index < 10 ? "0" + std::to_string(index) : std::to_string(index); }
}
bool GalacticMode::State::build_hero_action_hud() {
    if (!filesystem || !hud || !hud_fonts) return true;
    if (options.hud_provider && !options.hud_provider->hero_action(*this)) return false;
    hero_menu.blocker = memnew(Control);
    hud->add_child(hero_menu.blocker);
    hero_menu.blocker->set_name("hero_action_modal");
    hero_menu.blocker->set_z_index(10);
    hero_menu.blocker->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    // WGUI-47: menus lock world selection, including empty space and disabled rows.
    hero_menu.blocker->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    hero_menu.blocker->hide();
    for (auto& surface : hero_menu.surfaces) {
        const auto* shell = command_catalog.find("target_type_back"); // WGCH-188
        if (!shell) continue; // Missing authored menus stay unavailable.
        auto loaded = data::ui::load_shell_anchors(*filesystem, shell->text(data::ui::Field::model_name));
        if (!loaded) { failure = core::format_diagnostic(loaded.error()); return false; }
        surface.anchors = std::move(loaded).value().shell;
        surface.panel = memnew(EawrTacticalHud); hero_menu.blocker->add_child(surface.panel);
        EawrTacticalHud::Setup setup;
        setup.view = presentation::ui::hud_view_model(surface.anchors, command_catalog,
            presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem),
            presentation::ui::ShellOrigin::centre);
        // WGCH-188: this shell's _ALT<n> frames are the dialog's heights by row count, not faction variants. The tallest
        // holds the description and cancel where they are authored.
        const auto look = presentation::ui::hud_shell(surface.anchors, command_catalog, command_faction, true);
        std::optional<std::uint32_t> tallest;
        float tallest_height{};
        for (const auto& anchor : surface.anchors.anchors())
            if (anchor.alt.variant && anchor.rect.height > tallest_height) {
                tallest = anchor.alt.variant; tallest_height = anchor.rect.height;
            }
        for (const auto& mesh : look.meshes) {
            if (mesh.alternate && mesh.alternate != tallest) continue;
            const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
            if (!path) { failure = "hero action shell texture is absent: " + mesh.texture; return false; }
            auto texture = assets::load_texture(*filesystem, *path);
            if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
            std::string why;
            auto pixels = texture_image(texture.value(), why);
            if (pixels.is_null()) { failure = why; return false; }
            setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles, {}});
        }
        surface.panel->setup(std::move(setup), nullptr);
        surface.close_name = "h_close";
        const auto prefix = "h_";
        const auto add_label = [&](const std::string& name, const data::ui::CommandBarComponent& component) {
            auto* label = memnew(Label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            label->set_name(String::utf8((name + "_text").c_str()));
            const auto face = hud_fonts->resolve({std::string(component.text(data::ui::Field::font_name)),
                component.integer(data::ui::Field::font_point_size).value_or(0)}, "english");
            label->add_theme_font_override("font", hud_fonts->font(face));
            const auto tint = component.color(data::ui::Field::text_color)
                .value_or(component.color(data::ui::Field::color).value_or(data::ui::Rgba8{255,255,255,255}));
            label->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
            label->set_horizontal_alignment(component.flag(data::ui::Field::left_justified)
                ? HORIZONTAL_ALIGNMENT_LEFT : HORIZONTAL_ALIGNMENT_CENTER);
            if (name == "text_hero") label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
            surface.panel->add_child(label); surface.labels.emplace(name, label);
        };
        for (const auto& anchor : surface.anchors.anchors()) {
            const auto* component = command_catalog.find(anchor.name);
            if (!component || !anchor.alt.shown_for(presentation::ui::alt_variant(command_faction))) continue;
            const bool choice = option_name(anchor.name, prefix);
            const bool close = anchor.name == surface.close_name;
            if (choice || close) {
                auto* button = memnew(EawrHudButton);
                button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
                button->set_name(String::utf8(anchor.name.c_str()));
                HudButtonArt art{.normal = command_texture(first(*component, data::ui::Field::icon_texture_name)),
                    .hover = command_texture(first(*component, data::ui::Field::mouse_over_texture_name)),
                    .pressed = command_texture(first(*component, data::ui::Field::selected_texture_name)),
                    .disabled = command_texture(first(*component, data::ui::Field::disabled_texture_name)), .portrait = {}, .overlays = {}};
                if (choice) art.portrait = HudButtonArt::Portrait{{}, false, true};
                button->setup_art(art);
                surface.panel->add_child(button);
                if (close) {
                    surface.close = button; button->set_action([this] { close_hero_action(); });
                } else {
                    surface.choices.emplace_back(anchor.name, button);
                }
            }
            if (!component->text(data::ui::Field::font_name).empty()) add_label(anchor.name, *component);
        }
        // WGCH-188: the title, names and prices are bones of the shell with no mesh of their own.
        for (const auto& bone : surface.anchors.bones()) {
            const auto* component = command_catalog.find(bone.name);
            if (component && !surface.anchors.find(bone.name) && !component->text(data::ui::Field::font_name).empty())
                add_label(bone.name, *component);
        }
        std::sort(surface.choices.begin(), surface.choices.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (std::size_t index = 0; index < surface.choices.size(); ++index)
            surface.choices[index].second->set_action([this, index] { choose_hero_action(index); });
        surface.panel->hide();
    }
    return true;
}

void GalacticMode::State::close_hero_action() {
    if (!hero_menu.blocker || !hero_menu.blocker->is_visible_in_tree()) return;
    static_cast<void>(map_input.hud_event());
    if (!hero_menu.fixture && game && hero_menu.facts && hero_menu.facts->preparation)
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
            sim::galactic::CancelGalacticAbility{hero_menu.facts->actor, hero_menu.facts->preparation}, {}});
    hero_menu.closed = true; ++hero_menu.closes; hero_menu.blocker->hide();
}

std::size_t GalacticMode::State::hero_action_offered() const {
    const auto* facts = hero_menu.facts ? &*hero_menu.facts : nullptr;
    if (!facts) return 0;
    if (hero_menu.theft) return facts->theft ? facts->theft->size() : 0;
    if (hero_menu.sabotage) return facts->sabotage ? facts->sabotage->size() : 0;
    return facts->neutralization ? facts->neutralization->size() : 0;
}

void GalacticMode::State::choose_hero_action(std::size_t index) {
    if (!hero_menu.facts || !hero_menu.enabled || hero_menu.closed) return;
    const auto& facts = *hero_menu.facts;
    if (hero_menu.theft) {
        if (!facts.theft || index >= facts.theft->size() || !facts.theft->at(index).affordable) return;
        hero_menu.accepted_index = facts.theft->at(index).prepared_index;
        if (!hero_menu.fixture) {
            const auto& choice = facts.theft->at(index);
            if (!game || !choice.live_prepared) return;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::ExecutePreparedAbility{*choice.live_prepared}, {}});
        }
    } else if (hero_menu.sabotage) {
        // WGCH-73/96: the click queues the prepared structure and price; the host resolves both again.
        if (!facts.sabotage || index >= facts.sabotage->size() || !facts.sabotage->at(index).affordable) return;
        hero_menu.accepted_index = facts.sabotage->at(index).prepared_index;
        if (!hero_menu.fixture) {
            const auto& choice = facts.sabotage->at(index);
            if (!game || !choice.live_prepared) return;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::ExecutePreparedAbility{*choice.live_prepared}, {}});
        }
    } else {
        // WGCH-132: an invalid or unaffordable selection leaves the dialog open; accepted queues the stored option.
        if (!facts.neutralization || index >= facts.neutralization->size()
            || !facts.neutralization->at(index).affordable || !facts.neutralization->at(index).prepared.valid) return;
        hero_menu.accepted_index = facts.neutralization->at(index).prepared_index;
        if (!hero_menu.fixture) {
            const auto& choice = facts.neutralization->at(index);
            if (!game || !choice.live_prepared) return;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::ExecutePreparedAbility{*choice.live_prepared}, {}});
        }
    }
    static_cast<void>(map_input.hud_event()); ++hero_menu.choices_accepted;
    hero_menu.closed = true; hero_menu.blocker->hide();
}

void GalacticMode::State::update_hero_action_hud() {
    if (!hero_menu.blocker || !displayed_snapshot) return;
    const auto publication = hero_menu.fixture ? hero_menu.fixture : displayed_snapshot;
    const bool changed = publication != hero_menu.publication;
    const bool facts_changed = !hero_menu.publication || publication->hero_action_menus() != hero_menu.publication->hero_action_menus();
    const bool resized = camera.width != hero_menu.width || camera.height != hero_menu.height;
    const bool enabled = map_input.gates.hud_enabled && !map_input.gates.tutorial_locked;
    const bool gates_changed = enabled != hero_menu.enabled;
    if (!changed && !resized && !gates_changed) return; // No per-render-frame allocation.
    if (changed && !facts_changed && !resized && !gates_changed) { hero_menu.publication = publication; return; }
    hero_menu.width = camera.width; hero_menu.height = camera.height; hero_menu.enabled = enabled;
    if (changed) {
        hero_menu.publication = publication;
        if (facts_changed) {
            const auto* facts = publication->hero_action_menus();
            const auto previous = hero_menu.facts ? hero_menu.facts->preparation : 0;
            const auto current = facts ? facts->preparation : 0;
            if (hero_menu.fixture || previous != current) hero_menu.closed = false;
            hero_menu.facts.reset();
            if (facts && facts->player == galaxy->local.player) hero_menu.facts = *facts;
        }
        ++hero_menu.rebuilds;
    }
    const auto* facts = hero_menu.facts ? &*hero_menu.facts : nullptr;
    hero_menu.theft = facts && facts->theft.has_value();
    hero_menu.sabotage = facts && facts->sabotage.has_value();
    const bool supplied = facts && (hero_menu.theft ? facts->theft.has_value()
        : hero_menu.sabotage ? true : facts->neutralization.has_value());
    const auto offered = supplied ? (hero_menu.theft ? facts->theft->size()
        : hero_menu.sabotage ? facts->sabotage->size() : facts->neutralization->size()) : 0;
    // WGCH-188: the buttons are the whole grid, with no scrolling. A longer list still opens, showing what fits.
    const bool available = offered > 0 && hero_menu.surfaces[0].panel && !hero_menu.surfaces[0].choices.empty();
    hero_menu.blocker->set_visible(available && !hero_menu.closed && enabled);
    for (auto& surface : hero_menu.surfaces) {
        if (!surface.panel) continue;
        surface.panel->set_visible(available);
        const auto placement = surface.panel->placement();
        const auto position = [&](Control* control, std::string_view name) {
            const auto* anchor = surface.anchors.find(name);
            if (!anchor) return;
            const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
            control->set_position(Vector2(static_cast<float>(rect.x),static_cast<float>(rect.y)));
            control->set_size(Vector2(static_cast<float>(rect.width),static_cast<float>(rect.height)));
        };
        for (const auto& [name,label] : surface.labels) {
            position(label,name); label->set_text({}); label->hide(); label->set_modulate(Color(1.0F,1.0F,1.0F));
            const auto* component = command_catalog.find(name);
            label->add_theme_font_size_override("font_size", std::max(1, presentation::ui::font_pixel_height(
                component->integer(data::ui::Field::font_point_size).value_or(0),camera.height)));
        }
        const auto text = [&](const std::string& name, const std::string& value) {
            const auto found=surface.labels.find(name);
            if (found == surface.labels.end()) return;
            auto* label=found->second;
            label->set_text(String::utf8(value.c_str())); label->show();
            // A text bone has no mesh to give a box: the line centres on the bone's height, and on its x unless left-justified.
            const auto* bone = surface.anchors.find(name) ? nullptr : surface.anchors.find_bone(name);
            if (!bone) return;
            const auto point = presentation::ui::shell_point_to_screen(bone->origin.x, bone->origin.y, placement);
            const auto size = label->get_minimum_size();
            const bool left = label->get_horizontal_alignment() == HORIZONTAL_ALIGNMENT_LEFT;
            label->set_size(size);
            label->set_position(Vector2(static_cast<float>(point.x) - (left ? 0.0F : size.x/2.0F),
                static_cast<float>(point.y) - size.y/2.0F));
        };
        if (surface.close) {
            position(surface.close,surface.close_name); surface.close->set_hit_rect({{},surface.close->get_size()});
            text(surface.close_name,battle_text("TEXT_BUTTON_CANCEL")); surface.close->set_disabled(!enabled);
        }
        // A shared publication may contain only another ability's menu (e.g. destruction).
        // Its absent theft/neutralization list is not an empty vector to dereference.
        if (!facts || !supplied) continue;
        // Sabotage has no authored dialog title of its own here: structure rows and prices stand alone (named seam).
        if (!hero_menu.sabotage) {
            text("h_title",battle_text(hero_menu.theft ? "TEXT_SLICE_DIALOG_TITLE" : "TEXT_NEUTRALIZE_HERO_DIALOG_TITLE"));
            text("text_hero",battle_text(hero_menu.theft ? "TEXT_SLICE_DIALOG_DESCRIPTION" : "TEXT_NEUTRALIZE_HERO_DIALOG_DESCRIPTION"));
        }
        const auto count = offered;
        for (std::size_t index=0; index<surface.choices.size(); ++index) {
            const auto& [name,button] = surface.choices[index];
            position(button,name); button->set_hit_rect({{},button->get_size()});
            button->set_visible(index<count); button->set_disabled(true); button->set_icon_texture({}); button->set_tooltip_text({});
            if (index>=count) continue;
            sim::galactic::TypeId type{}; sim::math::Fixed price;
            bool admitted{};
            if (hero_menu.theft) {
                const auto& choice=facts->theft->at(index);
                type=choice.live_prepared ? choice.live_prepared->theft.technology : choice.prepared.technology;
                price=choice.live_prepared ? choice.live_prepared->price : choice.prepared.price; admitted=choice.affordable;
            } else if (hero_menu.sabotage) {
                const auto& choice=facts->sabotage->at(index);
                type=choice.structure_type; price=choice.price; admitted=choice.affordable;
            } else {
                const auto& choice=facts->neutralization->at(index);
                type=choice.prepared.siphon ? choice.source_type : choice.current_type;
                price=choice.prepared.price; admitted=choice.affordable && choice.prepared.valid;
            }
            std::string title;
            if (const auto lookup=production_names.find(type); lookup!=production_names.end())
                if (const auto art=production_art.find(lookup->second); art!=production_art.end()) {
                    title=art->second.text; button->set_icon_texture(command_texture(art->second.icon));
                    button->set_tooltip_text(String::utf8(art->second.encyclopedia.c_str()));
                }
            // Prices are facts. Unknown type display data stays blank. WGCH-188: a row that cannot be bought is grey.
            const auto name_label = "h_name_"+two(index), price_label = "h_price_"+two(index);
            text(name_label,title); text(price_label,"$ "+galactic_view::credit_balance(price));
            if (!admitted) for (const auto& row : {name_label,price_label})
                if (const auto found=surface.labels.find(row); found!=surface.labels.end()) found->second->set_modulate(Color(0.5F,0.5F,0.5F));
            const bool live_ready = hero_menu.fixture || (hero_menu.sabotage ? facts->sabotage->at(index).live_prepared.has_value()
                : hero_menu.theft ? facts->theft->at(index).live_prepared.has_value() : facts->neutralization->at(index).live_prepared.has_value());
            button->set_disabled(!enabled || !admitted || !live_ready);
        }
    }
}

void GalacticMode::State::write_hero_action_report(std::ostream& output) const {
    using galactic_detail::json;
    const auto* facts=hero_menu.facts ? &*hero_menu.facts : nullptr;
    output << "{\"source\": " << json(hero_menu.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"activation_seam\": \"campaign hero-action host\", \"execution_seam\": \"queued hero-action command provider\""
        << ", \"active\": " << (hero_menu.blocker && hero_menu.blocker->is_visible_in_tree() ? "true" : "false")
        << ", \"theft\": " << (hero_menu.theft ? "true" : "false") << ", \"offered\": " << hero_action_offered()
        << ", \"window\": " << hero_menu.surfaces[0].choices.size() << ", \"rebuilds\": " << hero_menu.rebuilds
        << ", \"choices_accepted\": " << hero_menu.choices_accepted << ", \"closes\": " << hero_menu.closes
        << ", \"accepted_index\": " << (hero_menu.accepted_index ? std::to_string(*hero_menu.accepted_index) : "null")
        << ", \"actor\": " << (facts ? facts->actor : 0) << ", \"planet\": " << (facts ? facts->planet : 0) << ", \"controls\": [";
    bool comma{};
    const auto control=[&](const std::string& name,const EawrHudButton* button) {
        if (!button || !button->is_visible()) return;
        const auto rect=button->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"enabled\": " << (!button->is_disabled() ? "true" : "false")
            << ", \"hit_rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma=true;
    };
    const auto& surface=hero_menu.surfaces[0];
    control(surface.close_name,surface.close);
    for (const auto& [name,button] : surface.choices) control(name,button);
    output << "], \"labels\": [";
    comma=false;
    for (const auto& [name,label] : surface.labels) {
        if (!label->is_visible() || label->get_text().is_empty()) continue;
        const auto rect=label->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"text\": " << json(label->get_text().utf8().get_data())
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma=true;
    }
    output << "], \"options\": [";
    if (facts && hero_menu.theft && facts->theft) for (std::size_t index=0;index<facts->theft->size();++index) {
        const auto& row=facts->theft->at(index);
        const auto technology = row.live_prepared ? row.live_prepared->theft.technology : row.prepared.technology;
        const auto price = row.live_prepared ? row.live_prepared->price : row.prepared.price;
        const auto chance = row.live_prepared ? row.live_prepared->theft.chance : row.prepared.chance;
        const auto absence = row.live_prepared ? row.live_prepared->absence : row.prepared.absence;
        const auto advance = row.live_prepared ? row.live_prepared->theft.advance : row.prepared.advance;
        output << (index ? ", " : "") << "{\"index\": " << row.prepared_index << ", \"technology\": " << technology
            << ", \"price_raw\": " << price.raw() << ", \"chance_raw\": " << chance.raw()
            << ", \"absence_raw\": " << absence.raw() << ", \"advance\": " << (advance ? "true" : "false")
            << ", \"affordable\": " << (row.affordable ? "true" : "false") << "}";
    }
    if (facts && !hero_menu.theft && facts->neutralization) for (std::size_t index=0;index<facts->neutralization->size();++index) {
        const auto& row=facts->neutralization->at(index);
        output << (index ? ", " : "") << "{\"index\": " << row.prepared_index << ", \"target\": " << row.prepared.target
            << ", \"company_type\": " << row.prepared.company_type << ", \"source_type\": " << row.source_type
            << ", \"siphon\": " << (row.prepared.siphon ? "true" : "false") << ", \"valid\": " << (row.prepared.valid ? "true" : "false")
            << ", \"price_raw\": " << row.prepared.price.raw() << ", \"actor_absence_raw\": " << row.prepared.actor_absence.raw()
            << ", \"target_absence_raw\": " << row.prepared.target_absence.raw()
            << ", \"affordable\": " << (row.affordable ? "true" : "false") << "}";
    }
    if (facts && facts->sabotage) for (std::size_t index=0;index<facts->sabotage->size();++index) {
        const auto& row=facts->sabotage->at(index);
        output << (index ? ", " : "") << "{\"index\": " << row.prepared_index << ", \"structure_type\": " << row.structure_type
            << ", \"price_raw\": " << row.price.raw() << ", \"affordable\": " << (row.affordable ? "true" : "false") << "}";
    }
    output << "], \"sabotage\": " << (hero_menu.sabotage ? "true" : "false") << "}";
}
} // namespace eawr::presentation::godot_backend
