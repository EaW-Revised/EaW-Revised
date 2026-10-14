#include "galactic_mode_internal.hpp"
#include "destruction_hud_fixture.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include <ostream>
using namespace godot;
namespace eawr::presentation::godot_backend {
bool GalacticMode::State::build_destruction_hud() {
    const auto* style = command_catalog.find("g_special_ability");
    if (!style || !hud || !hud_fonts) return true;
    if (options.campaign == galactic_view::synthetic_campaign && options.hud_fixture.starts_with("destruction-"))
        destruction_hud.fixture = galactic_view::destruction_fixture(galaxy->local.player, options.hud_fixture);
    auto& state = destruction_hud;
    state.slot = memnew(EawrHudButton); hud->add_child(state.slot);
    state.slot->set_name("galaxy_destruction_first_slot");
    const auto texture = [&](data::ui::Field field, std::size_t index = 0) {
        const auto names = style->list(field);
        return index < names.size() ? command_texture(names[index]) : Ref<Texture2D>{};
    };
    const auto background = texture(data::ui::Field::blank_texture_name);
    state.slot->set_texture_normal(background);
    // EawrHudButton ignores texture minimum sizes: derive this standalone
    // slot's dimensions from its authored art, like the other HUD adapters.
    if (background.is_valid()) state.authored_size = background->get_size();
    state.slot->set_texture_hover(texture(data::ui::Field::mouse_over_texture_name));
    // This authored texture table uses Rebel/Empire/pirates/raid order, not ALT order.
    const std::size_t faction = command_faction == presentation::ui::HudFaction::rebel ? 0
        : command_faction == presentation::ui::HudFaction::empire ? 1 : 2;
    state.slot->set_order_icon(texture(data::ui::Field::icon_alternate_texture_name, faction), false);
    state.slot->set_tooltip_text(String::utf8(battle_text("TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME").c_str()));
    // WGCH-154: a click is only consumed; this surface is neither a drag source
    // nor a confirmation button. EawrFleetDrag owns the left-release route.
    state.slot->set_action([this] { static_cast<void>(map_input.hud_event()); });
    const auto offset = style->vec2(data::ui::Field::offset).value_or(data::ui::Vec2{});
    const auto text_offset = style->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    state.authored_offset = Vector2(offset.x, -offset.y);
    state.text_offset = Vector2(text_offset.x, -text_offset.y);
    state.text = memnew(Label); state.slot->add_child(state.text);
    state.text->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    state.text->set_text(String::utf8(battle_text("TEXT_TOOLTIP_ABILITY_DESTROY_PLANET_NAME").c_str()));
    const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
        style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
    state.text->add_theme_font_override("font", hud_fonts->font(face));
    const auto tint = style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
    state.text->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,tint.a/255.0F));
    state.right_justified = style->flag(data::ui::Field::right_justified);
    state.text->set_horizontal_alignment(state.right_justified ? HORIZONTAL_ALIGNMENT_RIGHT : HORIZONTAL_ALIGNMENT_LEFT);
    state.slot->hide();
    return true;
}
void GalacticMode::State::update_destruction_hud() {
    auto& state = destruction_hud;
    if (!state.slot || !displayed_snapshot || !galaxy) return;
    const auto publication = state.fixture ? state.fixture : displayed_snapshot;
    if (publication != state.publication) {
        state.publication = publication; state.facts.reset(); ++state.rebuilds;
        if (const auto* menus = publication->hero_action_menus(); menus && menus->player == galaxy->local.player)
            state.facts = menus->destruction;
        // Preparation owns ranking, story/tutorial and fleet eligibility
        // (WGCH-152/153). Refusal is an execution fact, never a preview gate.
    }
    const auto& gates = map_input.gates;
    state.enabled = gates.galactic && gates.hud_enabled && gates.world_allowed() && !gates.tutorial_locked
        && !gates.action_menu && !gates.selection_locked && !tactical && !loading
        && !retreat_dialog_open && (!battle_dialog || !battle_dialog->visible())
        && displayed_snapshot->clock().pause_depth == 0;
    state.slot->set_visible(state.projected && state.enabled && state.facts && dragged_fleet == state.facts->source);
}
void GalacticMode::State::position_destruction_hud(galactic_view::ObjectId planet, Vector2 anchor, float scale, float alpha) {
    auto& state = destruction_hud;
    if (!state.slot || !state.facts || planet != state.facts->planet) return;
    state.projected = alpha > 0;
    // WGCH-DB-U01: existing authored HUD offset convention; exact special-slot
    // Offset/Text_Offset/right-justify mapping awaits retail pixel qualification.
    state.slot->set_position(anchor + state.authored_offset * scale);
    state.slot->set_size(state.authored_size * scale);
    state.slot->set_modulate(Color(1,1,1,alpha));
    if (const auto* style = command_catalog.find("g_special_ability"))
        state.text->add_theme_font_size_override("font_size", std::max(1,
            presentation::ui::font_pixel_height(style->integer(data::ui::Field::font_point_size).value_or(0), camera.height)));
    state.text->set_size(state.text->get_minimum_size());
    state.text->set_position(state.text_offset * scale - Vector2(state.right_justified ? state.text->get_size().x : 0, 0));
    update_destruction_hud();
}
bool GalacticMode::State::drop_destruction(galactic_view::ObjectId source, bool submit) {
    auto& state = destruction_hud;
    if (!state.enabled || !state.facts || source != state.facts->source || source != dragged_fleet) return false;
    if (!state.fixture) return false; // #1144: the queued ground-action host is not installed.
    if (submit) {
        ++state.accepts; state.accepted_source = state.facts->source; state.accepted_planet = state.facts->planet;
        // WGCH-155/156: publish original source/planet to the command seam.
        // No viewer payment, destruction, cleanup, refusal, victory or transport execution.
    }
    return true;
}
void GalacticMode::State::write_destruction_report(std::ostream& output) const {
    const auto& state = destruction_hud;
    const auto* facts = state.facts ? &*state.facts : nullptr;
    const auto rect = state.slot ? state.slot->get_global_rect() : Rect2{};
    output << "{\"source\": " << galactic_detail::json(state.fixture ? "controlled-observer-snapshot" : "campaign-snapshot")
        << ", \"preparation_seam\": \"ranked galaxy ground-ability host #1144\", \"execution_seam\": \"queued generic ground-action service #1144/#2272\""
        << ", \"geometry_qualification\": \"WGCH-DB-U01\", \"second_visible\": false"
        << ", \"visible\": " << (state.slot && state.slot->is_visible_in_tree() ? "true" : "false")
        << ", \"actor\": " << (facts ? facts->actor : 0) << ", \"source_id\": " << (facts ? facts->source : 0)
        << ", \"planet\": " << (facts ? facts->planet : 0) << ", \"refusal\": " << (facts ? static_cast<int>(facts->refusal) : 0)
        << ", \"rebuilds\": " << state.rebuilds << ", \"accepts\": " << state.accepts
        << ", \"accepted_source\": " << state.accepted_source << ", \"accepted_planet\": " << state.accepted_planet
        << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
}
}
