// The advanced map's orbiting ground-unit tray (GPD-14): the slot widgets and their per-frame update.
// Split out of galactic_radar_hud.cpp with no behaviour change. The model (which units, which slots) is land_tray_hud.hpp.
#include "galactic_mode_internal.hpp"
#include "land_tray_hud.hpp"
#include "fleet_drag.hpp"
#include "ground_hud.hpp"
#include "command_bar_scale.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "ui/input_routing.hpp"

#include "eawr/presentation/ui/layout.hpp"

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {
std::string first(const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto values = component.list(field);
    return values.empty() ? std::string{} : values.front();
}
}

void GalacticMode::State::build_land_tray() {
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        const auto name = galactic_view::land_tray_slot_name(slot);
        const auto* component = command_catalog.find(name);
        if (!component || !advanced_map_anchors.find(name)) continue;
        auto* button = memnew(EawrHudButton);
        button->set_name(String::utf8(name.c_str()));
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        // The slot never takes the engine pointer itself: the planet detail input (detail_hit, detail_pointer) owns
        // the press, drag and drop (GPD-22), and the rollover art is a separate non-blocking TextureRect shown
        // while the pointer is over the slot.
        button->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        button->set_order_icon({}, false);
        button->set_icon_fit(presentation::ui::IconFitMode::contain);
        button->hide();
        advanced_map_shell->add_child(button);
        land_fleet_buttons[slot] = button;
        auto* hover = memnew(TextureRect);
        hover->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        hover->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        hover->set_stretch_mode(TextureRect::STRETCH_SCALE);
        hover->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
        hover->set_texture(command_texture(first(*component, data::ui::Field::mouse_over_texture_name)));
        hover->hide();
        button->add_child(hover);
        land_fleet_hover[slot] = hover;
        auto* count = memnew(Label);
        count->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
            component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        const int font_size = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        count->add_theme_font_override("font", hud_fonts->command_font(face, font_size));
        count->add_theme_font_size_override("font_size", font_size);
        const auto colour = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255, 255, 255, 255});
        count->add_theme_color_override("font_color", Color(colour.r / 255.0F, colour.g / 255.0F,
            colour.b / 255.0F, colour.a / 255.0F));
        count->add_theme_constant_override("outline_size", component->flag(data::ui::Field::text_outline) ? 1 : 0);
        button->add_child(count);
        land_fleet_counts[slot] = count;
    }
}

void GalacticMode::State::update_advanced_land_tray(const presentation::ui::ShellPlacement& placement) {
    const auto facts = hud_facts.find(selected_planet);
    const bool owner = facts != hud_facts.end() && galaxy && facts->second.observed_owner == galaxy->local.player;
    // GPD-14 gates landing by the number already on the surface, not the orbiting stack size.
    const auto count = facts == hud_facts.end() ? std::optional<std::size_t>{} : facts->second.landed_companies;
    const auto capacity = facts == hud_facts.end() ? std::optional<std::int64_t>{} : facts->second.unit_capacity;
    const bool enabled = galactic_view::land_tray_enabled(owner, count, capacity);
    auto entries = galactic_view::land_tray_entries(fleet_intelligence.cards, selected_planet, enabled);
    const auto access = detail_access();
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        auto* button = land_fleet_buttons[slot];
        if (!button) continue;
        const auto* company = galactic_view::land_tray_entry(entries, slot);
        if (!company) { button->hide(); land_fleet_shown[slot].reset(); continue; }
        const auto name = galactic_view::land_tray_slot_name(slot);
        const auto* anchor = advanced_map_anchors.find(name);
        if (!anchor) { button->hide(); continue; }
        const auto pixels = presentation::ui::shell_to_screen(anchor->rect, placement);
        button->set_position({static_cast<float>(pixels.x), static_cast<float>(pixels.y)});
        button->set_size({static_cast<float>(pixels.width), static_cast<float>(pixels.height)});
        button->set_hit_rect({{}, button->get_size()});
        // Capacity and view-only access disable the tray.
        button->set_disabled(!company->enabled || !galactic_view::detail_can_edit(access));
        if (land_fleet_shown[slot] != std::optional<sim::galactic::TypeId>(company->type)) {
            land_fleet_shown[slot] = company->type;
            Ref<Texture2D> icon;
            if (const auto type = production_names.find(company->type); type != production_names.end())
                if (const auto art = production_art.find(type->second); art != production_art.end())
                    icon = command_texture(art->second.icon);
            button->set_icon_texture(icon);
        }
        if (auto* count_label = land_fleet_counts[slot]) {
            const auto* style = command_catalog.find(name);
            const auto offset = style ? style->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{}) : data::ui::Vec2{};
            count_label->set_position(button->get_size() * 0.5F + Vector2(offset.x, offset.y) * static_cast<float>(placement.scale));
            count_label->set_text(company->count > 1 ? String("x") + String::num_uint64(company->count) : String{});
        }
        button->show();
        if (auto* hover = land_fleet_hover[slot]; hover && button->get_viewport()) {
            const auto pointer = button->get_viewport()->get_mouse_position();
            hover->set_visible(company->enabled && access == galactic_view::PlanetDetailAccess::edit
                && button->get_global_rect().has_point(pointer));
        }
    }
    land_tray_shown = std::move(entries);
    if (tray_drag && *tray_drag >= land_tray_shown.size()) tray_cancel();
}

bool GalacticMode::State::tray_hit(const Vector2 point) const {
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        const auto* button = land_fleet_buttons[slot];
        if (button && slot < land_tray_shown.size() && button->is_visible_in_tree() && !button->is_disabled()
            && button->get_global_rect().has_point(point)) return true;
    }
    return false;
}

void GalacticMode::State::tray_cancel() {
    // Safe to call every frame: the shared drag art is touched only while a tray drag holds it.
    if (!tray_drag) { tray_moving = false; return; }
    tray_drag.reset(); tray_moving = false;
    if (ground_hud.input && ground_hud.input->drag_art) ground_hud.input->drag_art->cancel();
}

// GPD-22: press on an enabled tray slot, move, release over a unit pad. `target` is the pad under the pointer
// (building flag, index) as detail_pointer resolved it. Returns true when the tray owned the event.
bool GalacticMode::State::tray_pointer(const int kind, const Vector2 point,
    const std::optional<std::pair<bool, std::size_t>>& target) {
    if (kind == 0) {
        tray_cancel();
        for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
            const auto* button = land_fleet_buttons[slot];
            if (!button || slot >= land_tray_shown.size() || !button->is_visible_in_tree() || button->is_disabled()
                || !button->get_global_rect().has_point(point)) continue;
            tray_drag = slot;
            return true;
        }
        return false;
    }
    if (!tray_drag) return false;
    if (kind == 1) {
        if (!tray_moving) {
            tray_moving = true;
            if (audio) audio->ui_sound("GUI_Drag_Fleet");
            auto* button = land_fleet_buttons[*tray_drag];
            if (button && ground_hud.input && ground_hud.input->drag_art) {
                // GPD-33: the drag image is the unit's icon under the slot's mouse-over frame (the capture shows the
                // frame's corner brackets on the held icon); a slot holding one unit is emptied while it is held.
                auto* ghost = EawrFleetDrag::ghost_of(*button, point - button->get_global_position(), {}, false, false);
                const auto* component = command_catalog.find(galactic_view::land_tray_slot_name(*tray_drag));
                const auto frame = component ? command_texture(first(*component, data::ui::Field::mouse_over_texture_name)) : Ref<Texture2D>{};
                if (frame.is_valid() && ghost->get_child_count() > 0) {
                    if (auto* card = Object::cast_to<Control>(ghost->get_child(0))) {
                        auto* art = memnew(TextureRect);
                        art->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
                        art->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
                        art->set_stretch_mode(TextureRect::STRETCH_SCALE);
                        art->set_texture(frame);
                        art->set_position({});
                        art->set_size(button->get_size());
                        card->add_child(art);
                    }
                }
                ground_hud.input->drag_art->start(ghost);
                if (*tray_drag < land_tray_shown.size() && land_tray_shown[*tray_drag].count <= 1)
                    ground_hud.input->drag_art->lift_source(*button);
            }
        }
        return true;
    }
    const auto slot = *tray_drag;
    const bool moved = tray_moving;
    tray_cancel();
    // A release anywhere but a unit pad just ends the drag, as an abandoned pad move does.
    if (moved && target && !target->first) tray_drop(slot, target->second);
    return true;
}

// GPD-22: land the dragged tray unit on the pad and remember the marker; the pad's occupant goes to orbit (GPD-23).
// Everything the three commands admit is checked first (plan_tray_landing), then all three are submitted together,
// so a refusal never leaves the occupant launched without the landing. A refusal gives feedback and nothing else.
void GalacticMode::State::tray_drop(const std::size_t slot, const std::size_t pad_index) {
    const auto refuse = [this](galactic_view::TrayDropResult why) {
        ++tray_refusals; tray_last_refusal = why;
        if (audio) audio->negative_feedback();
    };
    const auto* entry = galactic_view::land_tray_entry(land_tray_shown, slot);
    if (!entry || !game || !galaxy || !published_snapshot || pad_index >= detail_layout.units.size())
        return refuse(galactic_view::TrayDropResult::no_unit);
    galactic_view::TrayDropFacts facts;
    facts.editable = galactic_view::detail_can_edit(detail_access());
    facts.deploy_allowed = pause_allows(presentation::GalacticPausedAction::surface_deploy);
    facts.layout_allowed = pause_allows(presentation::GalacticPausedAction::fleet_management);
    auto plan = galactic_view::plan_tray_landing(*published_snapshot, galaxy->local.player, selected_planet, *entry,
        detail_layout.units[pad_index], facts);
    if (plan.result != galactic_view::TrayDropResult::ok) return refuse(plan.result);
    std::vector<platform::LiveGameOrder> orders;
    for (auto& command : plan.commands) orders.push_back({sim::SeatId{galaxy->local.seat}, std::move(command), {}});
    submitted_orders += orders.size();
    game->submit_batch(std::move(orders));
    ++tray_drops; tray_last_refusal = galactic_view::TrayDropResult::ok;
    if (audio) audio->ui_sound("GUI_Drop_Fleet");
}

void GalacticMode::State::write_land_tray_report(std::ostream& output) const {
    output << "{\"drops\": " << tray_drops << ", \"refusals\": " << tray_refusals << ", \"last_refusal\": "
        << static_cast<int>(tray_last_refusal) << ", \"dragging\": " << (tray_drag ? "true" : "false") << ", \"slots\": [";
    std::size_t written{};
    for (std::size_t slot = 0; slot < land_fleet_buttons.size(); ++slot) {
        const auto* button = land_fleet_buttons[slot];
        if (!button || !button->is_visible_in_tree() || slot >= land_tray_shown.size()) continue;
        const auto rect = button->get_global_rect();
        const auto& entry = land_tray_shown[slot];
        output << (written++ ? ", " : "") << "{\"slot\": " << slot << ", \"type\": " << entry.type << ", \"count\": " << entry.count
            << ", \"unit\": " << entry.representative << ", \"enabled\": " << (button->is_disabled() ? "false" : "true")
            << ", \"rect\": [" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    // The drag image held by the pointer (GPD-33): where it is, whether it draws, and what it is made of.
    const auto* art = ground_hud.input ? ground_hud.input->drag_art : nullptr;
    const auto* ghost = art ? art->ghost() : nullptr;
    const auto ghost_rect = ghost ? ghost->get_global_rect() : Rect2{};
    std::size_t ghost_art = 0;
    if (ghost) for (std::int32_t i = 0; i < ghost->get_child_count(); ++i)
        if (const auto* card = Object::cast_to<Control>(ghost->get_child(i))) ghost_art += static_cast<std::size_t>(card->get_child_count());
    output << "], \"ghost\": {\"present\": " << (ghost ? "true" : "false") << ", \"visible\": "
        << (ghost && ghost->is_visible_in_tree() ? "true" : "false") << ", \"art\": " << ghost_art
        << ", \"drag_art_in_tree\": " << (art && art->is_visible_in_tree() ? "true" : "false")
        << ", \"position\": [" << (ghost ? ghost->get_global_position().x : 0.0F) << ", "
        << (ghost ? ghost->get_global_position().y : 0.0F) << "], \"rect\": [" << ghost_rect.position.x << ", "
        << ghost_rect.position.y << ", " << ghost_rect.size.x << ", " << ghost_rect.size.y << "]}, \"unit_pads\": [";
    written = 0;
    for (std::size_t slot = 0; slot < detail_unit_pads.size() && slot < detail_layout.units.size(); ++slot) {
        const auto* pad = detail_unit_pads[slot];
        if (!pad || !pad->is_visible_in_tree()) continue;
        const auto rect = pad->get_global_rect();
        const auto& model = detail_layout.units[slot];
        output << (written++ ? ", " : "") << "{\"slot\": " << slot << ", \"marker\": " << model.marker.record
            << ", \"object\": " << model.object.value_or(0) << ", \"rect\": [" << rect.position.x << ", " << rect.position.y
            << ", " << rect.size.x << ", " << rect.size.y << "]}";
    }
    output << "], \"lit_build_pads\": " << detail_pads_lit_builds << ", \"lit_unit_pads\": " << detail_pads_lit_units << "}";
}

} // namespace eawr::presentation::godot_backend
