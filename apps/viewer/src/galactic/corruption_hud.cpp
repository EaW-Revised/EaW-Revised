#include "galactic_mode_internal.hpp"
#include "corruption_hud_fixture.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <pugixml.hpp>
#include <algorithm>
#include <sstream>
#include <ostream>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
bool admitted(const sim::galactic::ObservedCorruptionChoice& row) {
    return row.affordable && row.units_satisfied && row.missions_satisfied && row.planets_satisfied;
}
bool same_menu(const sim::galactic::CorruptionMenu& a, const sim::galactic::CorruptionMenu& b) {
    if (a.player != b.player || a.actor != b.actor || a.planet != b.planet || a.ability != b.ability
        || a.choices.size() != b.choices.size()) return false;
    return std::equal(a.choices.begin(),a.choices.end(),b.choices.begin(),[](const auto& x,const auto& y) {
        const auto& p = x.prepared.choice; const auto& q = y.prepared.choice;
        const auto& r = p.requirements; const auto& s = q.requirements;
        return x.slot == y.slot && x.affordable == y.affordable && x.units_satisfied == y.units_satisfied
            && x.missions_satisfied == y.missions_satisfied && x.planets_satisfied == y.planets_satisfied
            && p.type == q.type && p.briefing == q.briefing && p.map == q.map && p.script == q.script
            && p.credits == q.credits && p.mode == q.mode && r.unit_types == s.unit_types
            && r.planet_types == s.planet_types && r.unit_count == s.unit_count
            && r.mission_count == s.mission_count && r.mission_type == s.mission_type;
    });
}
std::string suffix(unsigned slot) { return std::string(1, static_cast<char>('a' + slot - 1)); }
}
bool GalacticMode::State::build_corruption_hud() {
    if (!filesystem || !hud || !hud_fonts) return true;
    if (options.campaign == galactic_view::synthetic_campaign && galactic_view::corruption_fixture(options.hud_fixture)) {
        corruption_menu.fixture = galactic_view::corruption_fixture_snapshot(galaxy->local.player, options.hud_fixture);
        production_names.insert({{1000,"Rebel_Trooper_Team"},{1001,"Corellian_Corvette"},
            {2000,"Coruscant"},{2001,"Naboo"}});
        corruption_menu.briefing_keys = {{90,"TEXT_CORRUPTION_CORUSCANT_STORY_BRIBERY"},
            {91,"TEXT_CORRUPTION_CORUSCANT_STORY_BLACKMARKET"}};
    }
    const auto* shell = command_catalog.find("corruption_shell");
    if (!shell) return true;
    const auto bytes = filesystem->open("data/xml/gameconstants.xml");
    if (!bytes) { failure = core::format_diagnostic(bytes.error()); return false; }
    pugi::xml_document document;
    if (!document.load_buffer(bytes.value().data(), bytes.value().size(), pugi::parse_default | pugi::parse_doctype)) {
        failure = "corruption HUD requires valid GameConstants XML"; return false;
    }
    for (const auto node : document.children()) if (node.type() == pugi::node_doctype) {
        failure = "corruption HUD forbids a document type"; return false;
    }
    std::size_t list{};
    for (const auto tag : {"Corruption_Choice_Icon_Name", "Corruption_Choice_Name", "Corruption_Choice_Benefit",
        "Corruption_Mission_Requirement_Icon_Name", "Corruption_Choice_Encyclopedia"}) {
        std::istringstream stream(document.document_element().child(tag).text().as_string());
        std::string token;
        while (stream >> token) corruption_menu.display[list].push_back(token);
        ++list;
    }
    std::size_t marker{};
    for (const auto tag : {"Corruption_Encyclopedia_Incomplete", "Corruption_Encyclopedia_Complete"}) {
        std::istringstream stream(document.document_element().child(tag).text().as_string());
        stream >> corruption_menu.satisfaction_textures[marker++];
    }
    auto model = assets::load_model(*filesystem, "data/art/models/" + std::string(shell->text(data::ui::Field::model_name)));
    if (!model) { failure = core::format_diagnostic(model.error()); return false; }
    corruption_menu.anchors = data::ui::shell_anchors(model.value()).shell;
    // The authored titles and body text attach to bone-only points, which
    // the mesh-anchor loader intentionally does not turn into rectangles.
    auto player = presentation::animation::Player::create(model.value());
    if (!player) { failure = core::format_diagnostic(player.error()); return false; }
    auto pose = player.value().sample({});
    if (!pose) { failure = core::format_diagnostic(pose.error()); return false; }
    for (std::size_t index = 0; index < model.value().bones.size(); ++index) {
        const auto& name = model.value().bones[index].name;
        const auto* component = command_catalog.find(name);
        if (corruption_menu.anchors.find(name) || !component
            || component->text(data::ui::Field::font_name).empty()) continue;
        const auto& transform = pose.value().bones[index].model_asset;
        data::ui::ShellAnchor anchor;
        anchor.name = name; anchor.alt = data::ui::split_alt(name);
        anchor.rect = {transform[12], transform[13], 0, 0};
        anchor.origin = assets::Vec2f{transform[12], transform[13]};
        corruption_menu.anchors.add(std::move(anchor));
    }
    corruption_menu.blocker = memnew(Control); hud->add_child(corruption_menu.blocker);
    corruption_menu.blocker->set_name("corruption_modal"); corruption_menu.blocker->set_z_index(10);
    corruption_menu.blocker->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    corruption_menu.blocker->set_mouse_filter(Control::MOUSE_FILTER_STOP); corruption_menu.blocker->hide();
    corruption_menu.panel = memnew(EawrTacticalHud); corruption_menu.blocker->add_child(corruption_menu.panel);
    EawrTacticalHud::Setup setup;
    setup.view = presentation::ui::hud_view_model(corruption_menu.anchors, command_catalog,
        presentation::ui::alt_variant(command_faction), presentation::ui::vfs_shell_masks(*filesystem),
        presentation::ui::ShellOrigin::centre);
    const auto look = presentation::ui::hud_shell(corruption_menu.anchors, command_catalog, command_faction);
    for (const auto& mesh : look.meshes) {
        const auto path = presentation::ui::vfs_standalone_textures(*filesystem)(mesh.texture);
        if (!path) { failure = "corruption shell texture is absent: " + mesh.texture; return false; }
        auto texture = assets::load_texture(*filesystem, *path);
        if (!texture) { failure = core::format_diagnostic(texture.error()); return false; }
        std::string why; auto pixels = texture_image(texture.value(), why);
        if (pixels.is_null()) { failure = why; return false; }
        setup.meshes.push_back({mesh.blend, ImageTexture::create_from_image(pixels), mesh.triangles});
    }
    corruption_menu.panel->setup(std::move(setup), nullptr);
    for (const auto& anchor : corruption_menu.anchors.anchors()) {
        const auto* component = command_catalog.find(anchor.name);
        if (!component || !anchor.alt.shown_for(presentation::ui::alt_variant(command_faction))) continue;
        const bool choice = anchor.name.starts_with("corrupt_button_icon_") && anchor.name.size() == 21;
        const bool close = anchor.name == "corrupt_close";
        const bool requirement = anchor.name.starts_with("corrupt_icon_");
        if (choice || close || requirement) {
            auto* button = memnew(EawrHudButton);
            button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
            button->set_name(String::utf8(anchor.name.c_str()));
            if (!close) button->set_order_icon({},false);
            button->set_texture_normal(command_texture(first(*component,data::ui::Field::icon_texture_name)));
            button->set_texture_hover(command_texture(first(*component,data::ui::Field::mouse_over_texture_name)));
            button->set_texture_pressed(command_texture(first(*component,data::ui::Field::selected_texture_name)));
            button->set_texture_disabled(command_texture(first(*component,data::ui::Field::disabled_texture_name)));
            corruption_menu.panel->add_child(button);
            if (close) { corruption_menu.close = button; button->set_action([this] { close_corruption(); }); }
            else if (choice && anchor.name.back() >= 'a' && anchor.name.back() <= 'c') {
                const auto slot = static_cast<unsigned>(anchor.name.back() - 'a' + 1);
                corruption_menu.choices[slot-1] = button;
                button->set_action([this,slot] { choose_corruption(slot); });
            } else {
                // Passive requirement inspection still owns native hover and
                // pointer input; it never forwards a press to world selection.
                button->set_action([this] { static_cast<void>(map_input.hud_event()); });
            }
            corruption_menu.icons.emplace(anchor.name,button);
            if (requirement) {
                auto* indicator = memnew(TextureRect); indicator->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                indicator->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                button->add_child(indicator); corruption_menu.satisfaction.emplace(anchor.name,indicator);
            }
        }
        if (component->text(data::ui::Field::font_name).empty()) continue;
        auto* label = memnew(Label); label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->set_name(String::utf8((anchor.name+"_text").c_str()));
        const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
            component->integer(data::ui::Field::font_point_size).value_or(0)},"english");
        label->add_theme_font_override("font",hud_fonts->font(face));
        const auto tint = component->color(data::ui::Field::text_color)
            .value_or(component->color(data::ui::Field::color).value_or(data::ui::Rgba8{255,255,255,255}));
        label->add_theme_color_override("font_color",Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
        label->set_horizontal_alignment(component->flag(data::ui::Field::left_justified)
            ? HORIZONTAL_ALIGNMENT_LEFT : HORIZONTAL_ALIGNMENT_CENTER);
        if (anchor.name.starts_with("corrupt_text")) label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
        corruption_menu.panel->add_child(label); corruption_menu.labels.emplace(anchor.name,label);
    }
    return true;
}
void GalacticMode::State::close_corruption() {
    if (!corruption_menu.blocker || !corruption_menu.blocker->is_visible_in_tree()) return;
    static_cast<void>(map_input.hud_event()); corruption_menu.closed = true;
    if (!corruption_menu.fixture && game && corruption_menu.facts && corruption_menu.facts->preparation)
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
            sim::galactic::CancelGalacticAbility{corruption_menu.facts->actor, corruption_menu.facts->preparation}, {}});
    ++corruption_menu.closes; corruption_menu.blocker->hide();
}
void GalacticMode::State::choose_corruption(unsigned slot) {
    if (!corruption_menu.facts || !corruption_menu.enabled || corruption_menu.closed) return;
    const auto& rows = corruption_menu.facts->choices;
    const auto row = std::find_if(rows.begin(),rows.end(),[slot](const auto& value) { return value.slot == slot; });
    if (row == rows.end() || !admitted(*row)) return;
    if (!corruption_menu.fixture) {
        if (!game || !row->live_prepared) return;
        game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, sim::galactic::ExecutePreparedAbility{*row->live_prepared}, {}});
    }
    static_cast<void>(map_input.hud_event()); corruption_menu.accepted_slot = slot;
    ++corruption_menu.choices_accepted; corruption_menu.closed = true; corruption_menu.blocker->hide();
}
void GalacticMode::State::update_corruption_hud() {
    auto& state = corruption_menu;
    if (!state.blocker || !displayed_snapshot) return;
    const auto publication = state.fixture ? state.fixture : displayed_snapshot;
    const bool changed = !state.publication || publication->corruption_menu() != state.publication->corruption_menu();
    state.publication = publication;
    const bool enabled = map_input.gates.hud_enabled && !map_input.gates.tutorial_locked;
    if (!changed && state.width == camera.width && state.height == camera.height && enabled == state.enabled) return;
    state.width = camera.width; state.height = camera.height; state.enabled = enabled;
    if (changed) {
        state.publication = publication; ++state.rebuilds;
        const auto* facts = publication->corruption_menu();
        if (facts && facts->player == galaxy->local.player) {
            if (!state.facts || (facts->preparation ? state.facts->preparation != facts->preparation : !same_menu(*state.facts,*facts))) state.closed = false;
            state.facts = *facts;
        } else { state.facts.reset(); state.closed = false; }
    }
    const bool available = state.facts && !state.facts->choices.empty();
    state.blocker->set_visible(available && enabled && !state.closed);
    const auto placement = state.panel->placement();
    const auto position = [&](Control* control, std::string_view name) {
        const auto* anchor = state.anchors.find(name); if (!anchor) return;
        const auto rect = presentation::ui::shell_to_screen(anchor->rect,placement);
        control->set_position(Vector2(static_cast<float>(rect.x),static_cast<float>(rect.y)));
        control->set_size(Vector2(static_cast<float>(rect.width),static_cast<float>(rect.height)));
    };
    for (const auto& [name,label] : state.labels) {
        position(label,name); label->set_text({}); label->hide();
        const auto* component = command_catalog.find(name);
        if (name.starts_with("corrupt_text")) {
            // Text components author the wrap width in the second Size field;
            // Godot derives the height from the localized paragraph and font.
            const auto size = component->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
            if (size.y > 0) label->set_size(Vector2(size.y * static_cast<float>(placement.scale), 0));
        }
        const auto offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
        label->set_position(label->get_position()+Vector2(offset.x,offset.y)*static_cast<float>(placement.scale));
        label->add_theme_font_size_override("font_size",std::max(1,presentation::ui::font_pixel_height(
            component->integer(data::ui::Field::font_point_size).value_or(0),camera.height)));
    }
    for (const auto& [name,button] : state.icons) {
        position(button,name); button->set_hit_rect({{},button->get_size()}); button->hide();
        button->set_icon_texture({}); button->set_tooltip_text({}); button->set_disabled(true);
    }
    for (const auto& [name,indicator] : state.satisfaction) { static_cast<void>(name); indicator->hide(); }
    const auto text = [&](const std::string& name, const std::string& value) {
        if (const auto found = state.labels.find(name); found != state.labels.end()) {
            found->second->set_text(String::utf8(value.c_str())); found->second->set_visible(!value.empty());
        }
    };
    if (state.close) { state.close->show(); state.close->set_disabled(!enabled); text("corrupt_close",battle_text("TEXT_BUTTON_CLOSE")); }
    if (!available) return;
    text("corrupt_planet_title00",battle_text("TEXT_CORRUPTION_TITLE"));
    if (const auto planet = hud_facts.find(state.facts->planet); planet != hud_facts.end())
        text("corrupt_planet_title01",planet->second.display_name);
    const auto display = [&](std::size_t list, sim::galactic::corruption_actions::Type type) {
        const auto ordinal = static_cast<std::size_t>(type);
        return ordinal > 0 && ordinal <= state.display[list].size() ? state.display[list][ordinal-1] : std::string{};
    };
    const auto art = [&](sim::galactic::TypeId type) -> const galactic_view::ProductionArt* {
        const auto name = production_names.find(type); if (name == production_names.end()) return nullptr;
        const auto found = production_art.find(name->second); return found == production_art.end() ? nullptr : &found->second;
    };
    for (const auto& row : state.facts->choices) {
        if (row.slot < 1 || row.slot > 3) continue;
        const auto tail = suffix(row.slot); const auto& choice = row.prepared.choice;
        auto* button = state.choices[row.slot-1]; if (!button) continue;
        button->show(); button->set_icon_texture(command_texture(display(0,choice.type)));
        button->set_tooltip_text(String::utf8(battle_text(display(4,choice.type)).c_str()));
        button->set_disabled(!enabled || !admitted(row) || (!state.fixture && !row.live_prepared));
        text("corrupt_title_"+tail,battle_text(display(1,choice.type)));
        text("corrupt_cost_"+tail,"$ "+galactic_view::credit_balance(choice.credits));
        text("corrupt_text00"+tail,battle_text(display(2,choice.type)));
        // The controlled fixture supplies this map. Live content-ID resolution
        // remains a named producer seam; missing keys stay blank.
        if (row.live_choice) text("corrupt_text01"+tail,battle_text(row.live_choice->briefing));
        else if (const auto key = state.briefing_keys.find(choice.briefing); key != state.briefing_keys.end())
            text("corrupt_text01"+tail,battle_text(key->second));
        // GHUD-C08 text-flow seam: controlled paragraphs retain authored
        // points unless their wrapped heights overlap. Retail clipping,
        // scrolling and font reduction remain unqualified by a capture.
        const auto benefit = state.labels.find("corrupt_text00"+tail);
        const auto briefing = state.labels.find("corrupt_text01"+tail);
        if (benefit != state.labels.end() && briefing != state.labels.end()
            && benefit->second->is_visible() && briefing->second->is_visible()) {
            auto point = briefing->second->get_position();
            point.y = std::max(point.y, benefit->second->get_position().y
                + benefit->second->get_minimum_size().y);
            briefing->second->set_position(point);
        }
        const auto& requirements = choice.requirements;
        std::size_t icon{};
        const auto requirement = [&](const std::string& texture, const std::string& title, unsigned count, bool satisfied) {
            const auto name = "corrupt_icon_" + (icon < 10 ? std::string("0") : std::string{}) + std::to_string(icon) + tail;
            ++icon;
            const auto found = state.icons.find(name); if (found == state.icons.end()) return;
            found->second->show(); found->second->set_icon_texture(command_texture(texture));
            found->second->set_tooltip_text(String::utf8(title.c_str()));
            text(name,count ? std::to_string(count) : std::string{});
            // Separate satisfaction from affordability; requirement icons keep
            // their identity while the native disabled state conveys admission.
            found->second->set_disabled(!satisfied);
            if (const auto indicator = state.satisfaction.find(name); indicator != state.satisfaction.end()) {
                const auto marker_texture = command_texture(state.satisfaction_textures[satisfied ? 1 : 0]);
                if (marker_texture.is_valid()) {
                    indicator->second->set_texture(marker_texture);
                    const auto scale = static_cast<float>(placement.scale);
                    const Vector2 size(static_cast<float>(marker_texture->get_width())*scale,static_cast<float>(marker_texture->get_height())*scale);
                    indicator->second->set_size(size);
                    indicator->second->set_position(Vector2(found->second->get_size().x-size.x,0));
                    indicator->second->show();
                }
            }
        };
        if (row.live_choice) {
            const auto& live = *row.live_choice;
            if (live.unit_count) for (const auto type : live.unit_types) if (const auto* value = art(type))
                requirement(value->icon,battle_text(value->text),live.unit_count,row.units_satisfied);
            if (live.mission_count && live.required_mission_type != sim::galactic::corruption_actions::Type::none)
                requirement(display(3,live.required_mission_type),battle_text(display(1,live.required_mission_type)),live.mission_count,row.missions_satisfied);
            for (const auto type : live.planet_types) if (const auto* value = art(type)) requirement(value->icon,battle_text(value->text),1,row.planets_satisfied);
        } else {
        if (requirements.unit_count) for (const auto type : requirements.unit_types) if (const auto* value = art(type))
            requirement(value->icon,battle_text(value->text),requirements.unit_count,row.units_satisfied);
        if (requirements.mission_count && requirements.mission_type != sim::galactic::corruption_actions::Type::none)
            requirement(display(3,requirements.mission_type),battle_text(display(1,requirements.mission_type)),
                requirements.mission_count,row.missions_satisfied);
        for (const auto type : requirements.planet_types) if (const auto* value = art(type))
            requirement(value->icon,battle_text(value->text),0,row.planets_satisfied);
        }
    }
}
void GalacticMode::State::write_corruption_report(std::ostream& output) const {
    using galactic_detail::json;
    using galactic_detail::utf8;
    const auto& state = corruption_menu;
    output << "{\"source\": " << json(state.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"activation_seam\": \"campaign corruption host\", \"execution_seam\": \"queued corruption command provider\""
        << ", \"briefing_seam\": \"campaign content-ID/text-key resolver\""
        << ", \"requirement_display_seam\": \"campaign content-ID/art resolver and authored pane capacity\""
        << ", \"text_flow_seam\": \"GHUD-C08 retail text overflow; controlled minimal non-overlap fallback\""
        << ", \"active\": " << (state.blocker && state.blocker->is_visible_in_tree() ? "true" : "false")
        << ", \"rebuilds\": " << state.rebuilds << ", \"choices_accepted\": " << state.choices_accepted
        << ", \"closes\": " << state.closes << ", \"accepted_slot\": " << (state.accepted_slot ? std::to_string(*state.accepted_slot) : "null")
        << ", \"controls\": [";
    bool comma{};
    for (const auto& [name,button] : state.icons) {
        if (!button->is_visible()) continue;
        const auto rect = button->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"enabled\": " << (!button->is_disabled() ? "true" : "false")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma = true;
    }
    output << "], \"options\": ["; comma = false;
    if (state.facts) for (const auto& row : state.facts->choices) {
        const auto& choice = row.prepared.choice;
        output << (comma ? ", " : "") << "{\"slot\": " << row.slot << ", \"type\": " << static_cast<unsigned>(choice.type)
            << ", \"briefing\": " << choice.briefing << ", \"credits_raw\": " << choice.credits.raw()
            << ", \"affordable\": " << (row.affordable ? "true" : "false") << ", \"units_satisfied\": " << (row.units_satisfied ? "true" : "false")
            << ", \"missions_satisfied\": " << (row.missions_satisfied ? "true" : "false") << ", \"planets_satisfied\": " << (row.planets_satisfied ? "true" : "false") << "}";
        comma = true;
    }
    output << "], \"labels\": ["; comma = false;
    for (const auto& [name,label] : state.labels) if (label->is_visible()) {
        const auto rect = label->get_global_rect();
        output << (comma ? ", " : "") << "{\"name\": " << json(name) << ", \"text\": " << json(utf8(label->get_text()))
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
        comma = true;
    }
    output << "]}";
}
} // namespace eawr::presentation::godot_backend
