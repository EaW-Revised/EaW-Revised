#include "galactic_mode_internal.hpp"
#include "ui/tactical_hud_internal.hpp"
#include "eawr/presentation/ui/layout.hpp"
#include "eawr/presentation/animation/animation.hpp"
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <charconv>
#include <cmath>
#include <ostream>
#include <string_view>

using namespace godot;
namespace eawr::presentation::godot_backend {
namespace {
Ref<Texture2D> texture(GalacticMode::State& state, const data::ui::CommandBarComponent& component, data::ui::Field field) {
    const auto names = component.list(field);
    return names.empty() ? Ref<Texture2D>{} : state.command_texture(names.front());
}
// GHUD-S68: the shell's land special-structure slot components; the slot index is the name suffix.
constexpr std::string_view structure_prefix = "Planet_structure_";
bool structure_slot(std::string_view name, std::size_t& slot) {
    if (!name.starts_with(structure_prefix)) return false;
    const auto digits = name.substr(structure_prefix.size());
    const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), slot);
    return parsed.ec == std::errc{} && parsed.ptr == digits.data() + digits.size();
}
// GHUD-S69: the pads' company layout of one planet from the viewer's remembered drops.
galactic_view::PadLayout pad_layout(const GroundHudState& hud, const galactic_view::ObjectId planet,
    const galactic_view::GroundRoster& roster) {
    static const std::map<galactic_view::ObjectId, std::size_t> none;
    std::size_t pads{};
    for (const auto& [name, button] : hud.buttons) if (name.starts_with("Pad_")) ++pads;
    const auto found = hud.pad_slots.find(planet);
    return galactic_view::place_pads(roster.companies, found == hud.pad_slots.end() ? none : found->second, pads);
}
bool selectable(const GalacticMode::State& state) {
    const auto& gates = state.map_input.gates;
    return gates.galactic && gates.hud_enabled && gates.world_allowed() && !gates.tutorial_locked
        && !gates.selection_locked && !gates.action_menu && !state.tactical && !state.loading
        && !state.retreat_dialog_open && (!state.battle_dialog || !state.battle_dialog->visible());
}
std::string sale_caption(const GalacticMode::State& state, const galactic_view::GroundStructure& structure) {
    if (!structure.sale_allowed) return " " + state.ground_hud.busy_text + " ";
    const auto type = state.production_names.find(structure.type);
    const auto art = type == state.production_names.end() ? state.production_art.end() : state.production_art.find(type->second);
    const auto title = art == state.production_art.end() ? std::string{} : art->second.text;
    return " " + state.ground_hud.sale_text + " " + title + " ($ "
        + std::to_string(structure.refund.trunc_to_integer()) + ") ";
}
void layout_sale(GalacticMode::State& state, bool place = false) {
    auto& hud = state.ground_hud;
    const auto* component = state.command_catalog.find("g_ground_sell");
    if (!component || !hud.sale || !hud.sale_label || !state.camera.width || !state.camera.height) return;
    const float viewport_scale = static_cast<float>(state.camera.height) / 768.0F;
    const float authored_scale = static_cast<float>(component->number(data::ui::Field::scale).value_or(1.0));
    const auto art = texture(state, *component, data::ui::Field::icon_texture_name);
    const auto text_size = hud.sale_label->get_minimum_size() * authored_scale;
    // GHUD-S84: retail replaces the quad width with the text width, retaining its authored texture height.
    const Vector2 size(text_size.x, art.is_valid() ? art->get_height() * viewport_scale * authored_scale : text_size.y);
    hud.sale->set_size(size);
    static_cast<EawrHudButton*>(hud.sale)->set_hit_rect({{}, size});
    hud.sale_label->set_scale(Vector2(authored_scale, authored_scale));
    hud.sale_label->set_size(hud.sale_label->get_minimum_size());
    const auto text_offset = component->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    hud.sale_label->set_position(Vector2(text_offset.x * viewport_scale,
        (size.y - text_size.y) * 0.5F - text_offset.y * viewport_scale));
    if (place) {
        auto offset = component->vec2(data::ui::Field::default_offset).value_or(data::ui::Vec2{});
        if (static_cast<float>(state.camera.width) / static_cast<float>(state.camera.height) >= 1.75F)
            offset = component->vec2(data::ui::Field::default_offset_widescreen).value_or(offset);
        Vector2 centre(offset.x, offset.y);
        if (offset.x < 0.0F) {
            // Negative authored X requests one-shot pointer placement, with the same conversion gap as GHUD-S72.
            auto corner = hud.sale_centre + Vector2(8192.0F / static_cast<float>(state.camera.width) * viewport_scale,
                6144.0F / static_cast<float>(state.camera.height) * 3.0F * viewport_scale);
            corner = Vector2(std::floor(corner.x + 0.5F) - 0.5F, std::ceil(corner.y - 0.5F) + 0.5F);
            centre = corner + size * 0.5F;
        }
        hud.sale_centre = centre;
    }
    // Caption/size refreshes preserve the stored centre, as does an explicit authored screen position.
    hud.sale->set_position(hud.sale_centre - size * 0.5F);
}
}
EawrGroundInput::EawrGroundInput() { set_mouse_filter(MOUSE_FILTER_PASS); set_focus_mode(FOCUS_NONE); }
bool EawrGroundInput::_has_point(const Vector2& point) const { return hit && hit(point); }
void EawrGroundInput::_gui_input(const Ref<InputEvent>& input) {
    if (const auto* button = input.is_valid() ? Object::cast_to<InputEventMouseButton>(input.ptr()) : nullptr;
        button && button->get_button_index() == MOUSE_BUTTON_RIGHT) {
        if (right_event && right_event(button->get_position(), button->is_pressed())) accept_event();
        return; // GHUD-S84: foreign/unsellable structure right presses retain the ordinary map route.
    }
    accept_event(); // Even rejected/empty/foreign slots cannot select the world.
    if (!event) return;
    if (const auto* button = input.is_valid() ? Object::cast_to<InputEventMouseButton>(input.ptr()) : nullptr) {
        if (button->get_button_index() == MOUSE_BUTTON_LEFT)
            event(button->is_pressed() ? 0 : 2, button->get_position(), button->is_ctrl_pressed(), button->is_double_click());
    } else if (const auto* motion = input.is_valid() ? Object::cast_to<InputEventMouseMotion>(input.ptr()) : nullptr)
        event(1, motion->get_position(), motion->is_ctrl_pressed(), false);
    queue_redraw();
}
void EawrGroundInput::_notification(int what) {
    if (what != NOTIFICATION_DRAW || !rectangle) return;
    if (const auto rect = rectangle()) {
        draw_rect(*rect, Color(1,1,1,0.2F));
        draw_rect(*rect, Color(1,1,1,1), false, 3);
    }
}

bool GalacticMode::State::open_object_sale(sim::EntityId object, Vector2 point) {
    ground_hud.sale_object = 0;
    if (ground_hud.sale) ground_hud.sale->hide();
    // The sale gates a command, so it reads the truth publication, never the reveal view.
    if (!object || !selectable(*this) || !published_snapshot || !galaxy || !ground_hud.sale
        || !published_snapshot->production()) return false;
    const auto& snapshot = *published_snapshot;
    const auto sale = sim::galactic::object_sale(*snapshot.production(), snapshot.sale_types(), snapshot.actor_records(),
        snapshot.fleets(), snapshot.control(), snapshot.movement(), galaxy->local.player, selected_planet, object);
    if (sale.admission != sim::galactic::ProductionAdmission::allowed
        && sale.admission != sim::galactic::ProductionAdmission::locked) return false;
    const galactic_view::GroundStructure caption{object, sale.type, galaxy->local.player,
        sale.admission == sim::galactic::ProductionAdmission::allowed, true, sale.refund};
    ground_hud.sale_object = object; ground_hud.sale_planet = selected_planet; ground_hud.sale_centre = point;
    ground_hud.sale_label->set_text(String::utf8(sale_caption(*this, caption).c_str()));
    layout_sale(*this, true);
    ground_hud.sale->set_disabled(!caption.sale_allowed); ground_hud.sale->show();
    ++ground_hud.sale_opened; return true;
}

bool GalacticMode::State::build_ground_hud() {
    if (!fleet_organize) return true;
    // H052 / WGUI-41: the ground count can attach to a bone-only point.
    if (const auto* count_style = command_catalog.find("Text_Land_Fleet");
        count_style && !fleet_anchors.find("Text_Land_Fleet")) {
        auto model = assets::load_model(*filesystem, fleet_anchors.model_path());
        if (!model) { failure = core::format_diagnostic(model.error()); return false; }
        auto player = presentation::animation::Player::create(model.value());
        if (!player) { failure = core::format_diagnostic(player.error()); return false; }
        auto pose = player.value().sample({});
        if (!pose) { failure = core::format_diagnostic(pose.error()); return false; }
        for (std::size_t index = 0; index < model.value().bones.size(); ++index) {
            if (command_catalog.find(model.value().bones[index].name) != count_style) continue;
            const auto& transform = pose.value().bones[index].model_asset;
            data::ui::ShellAnchor anchor;
            anchor.name = "Text_Land_Fleet";
            anchor.rect = {transform[12], transform[13], 0, 0};
            anchor.origin = assets::Vec2f{transform[12], transform[13]};
            fleet_anchors.add(std::move(anchor));
            break;
        }
    }
    for (const auto& anchor : fleet_anchors.anchors()) {
        const auto& name = anchor.name;
        std::size_t slot{};
        const bool structure = structure_slot(name, slot);
        if (!name.starts_with("Pad_") && name != "Slot_00" && name != "b_underground" && !structure) continue;
        const auto* component = command_catalog.find(name);
        if (!component) continue;
        auto* button = memnew(EawrHudButton);
        button->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        fleet_organize->add_child(button);
        button->set_name(String::utf8(name.c_str()));
        button->setup_art({.normal = texture(*this, *component, data::ui::Field::blank_texture_name),
            .hover = texture(*this, *component, data::ui::Field::mouse_over_texture_name),
            .pressed = texture(*this, *component, data::ui::Field::selected_texture_name),
            // A structure slot's icon is its structure's, set per publication (GHUD-S68); never the authored
            // unavailable-slot art, which retail shows only for pads of an inaccessible surface.
            .portrait = HudButtonArt::Portrait{structure || name == "b_underground" ? Ref<Texture2D>{}
                : texture(*this, *component, data::ui::Field::icon_texture_name), false, !structure}});
        ground_hud.buttons.emplace(name, button);
        if (name == "Slot_00") button->set_action([this] { static_cast<void>(request_ground_land(true)); });
        else button->set_action([] {});
    }
    ground_hud.count = memnew(Label); fleet_organize->add_child(ground_hud.count);
    ground_hud.count->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    ground_hud.count->hide();
    if (const auto* style = command_catalog.find("Text_Land_Fleet")) {
        const auto face = hud_fonts->resolve({std::string(style->text(data::ui::Field::font_name)),
            style->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        ground_hud.count->add_theme_font_override("font", hud_fonts->font(face));
        ground_hud.count->add_theme_font_size_override("font_size", std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height)));
        const auto colour = style->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
        ground_hud.count->add_theme_color_override("font_color", Color(colour.r / 255.0F, colour.g / 255.0F, colour.b / 255.0F, colour.a / 255.0F));
        ground_hud.count->add_theme_constant_override("outline_size", style->flag(data::ui::Field::text_outline) ? 1 : 0);
    }
    auto* input = memnew(EawrGroundInput); hud->add_child(input); ground_hud.input = input;
    input->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    input->drag_art = memnew(EawrFleetDrag); input->add_child(input->drag_art);
    input->drag_art->receiver({}, {}, {});
    input->drag_art->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    input->hit = [this](Vector2 point) {
        if (!fleet_organize->is_visible_in_tree() || dragged_fleet) return false;
        if (map_input.box_start) return true;
        for (const auto& [name, button] : ground_hud.buttons)
            if ((name.starts_with("Pad_") || name.starts_with(structure_prefix))
                && button->is_visible_in_tree() && button->get_global_rect().has_point(point)) return true;
        return false;
    };
    input->rectangle = [this]() -> std::optional<Rect2> {
        if (!map_input.box_start || !map_input.box_end) return {};
        const auto a = *map_input.box_start, b = *map_input.box_end;
        return Rect2(std::min(a.x,b.x), std::min(a.y,b.y), std::abs(a.x-b.x), std::abs(a.y-b.y));
    };
    input->event = [this](int kind, Vector2 point, bool control, bool twice) { ground_pointer(kind, point, control, twice); };
    input->right_event = [this](Vector2 point, bool pressed) {
        if (pressed) {
            ground_hud.sale_object = 0;
            if (ground_hud.sale) ground_hud.sale->hide();
        }
        if (!selectable(*this) || !galaxy) return false;
        const auto roster = ground_hud.rosters.find(selected_planet);
        if (roster == ground_hud.rosters.end()) return false;
        for (const auto& [name, button] : ground_hud.buttons) {
            if (name.starts_with("Pad_") && button->is_visible_in_tree() && button->get_global_rect().has_point(point)) {
                if (pressed) {
                    for (const auto& widget : ground_hud.widgets) {
                        const auto pad = "Pad_" + (widget.id < 10 ? std::string("0") : std::string{}) + std::to_string(widget.id);
                        if (pad == name && widget.visible && widget.object)
                            static_cast<void>(open_object_sale(widget.object, point));
                    }
                }
                return true; // Pad right-button ownership remains in the GUI.
            }
            std::size_t slot{};
            if (!structure_slot(name, slot) || !button->is_visible_in_tree() || !button->get_global_rect().has_point(point)
                || slot >= roster->second.structures.size()) continue;
            const auto& structure = roster->second.structures[slot];
            if (structure.owner != galaxy->local.player || !structure.sale_available) return false;
            if (pressed) ground_pointer(3, point, false, false);
            return true;
        }
        return false;
    };
    if (const auto* component = command_catalog.find("g_ground_sell")) {
        auto* sale = memnew(EawrHudButton); hud->add_child(sale); ground_hud.sale = sale;
        sale->set_name("g_ground_sell"); sale->set_z_index(static_cast<int>(component->integer(data::ui::Field::base_layer).value_or(0)));
        sale->setup_art({.normal = texture(*this, *component, data::ui::Field::icon_texture_name),
            .hover = texture(*this, *component, data::ui::Field::mouse_over_texture_name),
            .disabled = texture(*this, *component, data::ui::Field::disabled_texture_name)});
        sale->set_sounds(*component, [this](const std::string& cue) { if (audio) audio->ui_sound(cue); });
        ground_hud.sale_label = memnew(Label); sale->add_child(ground_hud.sale_label);
        auto* label = ground_hud.sale_label; label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        const auto face = hud_fonts->resolve({std::string(component->text(data::ui::Field::font_name)),
            component->integer(data::ui::Field::font_point_size).value_or(0)}, "english");
        const int pixels = std::max(1, presentation::ui::font_pixel_height(face.point_size, camera.height));
        label->add_theme_font_override("font", hud_fonts->command_font(face, pixels));
        label->add_theme_font_size_override("font_size", pixels);
        const auto tint = component->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{255,255,255,255});
        label->add_theme_color_override("font_color", Color(tint.r/255.0F,tint.g/255.0F,tint.b/255.0F,1.0F));
        label->add_theme_constant_override("outline_size", component->flag(data::ui::Field::text_outline) ? 1 : 0);
        auto text = data::ui::load_language_text_database(*filesystem, "english");
        const auto localized = [&](std::string_view key) {
            const auto* entry = text ? text.value().find(key) : nullptr;
            return entry ? data::ui::to_utf8(entry->value) : std::string(key);
        };
        ground_hud.sale_text = localized("TEXT_BUTTON_SELL");
        ground_hud.busy_text = localized("TEXT_PRODUCTION_IN_PROGRESS01");
        sale->set_action([this] {
            if (!selectable(*this) || !game || !galaxy || !ground_hud.sale_object || ground_hud.sale_planet != selected_planet) return;
            game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat},
                sim::galactic::SellObject{selected_planet, ground_hud.sale_object}, {}});
            if (audio) {
                audio->ui_sound("GUI_Structure_Sold");
                if (published_snapshot) {
                    const auto type = published_snapshot->current_type(ground_hud.sale_object);
                    const auto name = type ? production_names.find(type.value()) : production_names.end();
                    const auto art = name == production_names.end() ? production_art.end() : production_art.find(name->second);
                    if (art != production_art.end() && !art->second.sold_sound.empty()) audio->ui_sound(art->second.sold_sound);
                }
            }
            ++submitted_orders; ++ground_hud.sales_submitted;
            ground_hud.sale_object = 0; ground_hud.sale->hide();
        });
        sale->hide();
    }
    // Drag receiver must continue to receive fleet drops over ground slots.
    if (fleet_drag_receiver) hud->move_child(fleet_drag_receiver, hud->get_child_count() - 1);
    return true;
}

void GalacticMode::State::ground_pointer(int kind, Vector2 point, bool control, bool twice) {
    if (!selectable(*this)) {
        ground_hud.input->drag_art->cancel();
        map_input.cancel_pointer(); ground_hud.pressed = 0; ground_hud.pressed_object = 0; return;
    }
    if (kind == 0 || kind == 3) {
        ground_hud.sale_object = 0;
        if (ground_hud.sale) ground_hud.sale->hide();
        for (const auto& [name, button] : ground_hud.buttons) {
            std::size_t slot{};
            if (!structure_slot(name, slot) || !button->is_visible_in_tree() || !button->get_global_rect().has_point(point)) continue;
            const auto roster = ground_hud.rosters.find(selected_planet);
            if (!ground_hud.sale || roster == ground_hud.rosters.end() || slot >= roster->second.structures.size()) return;
            const auto& structure = roster->second.structures[slot];
            if (!galaxy || structure.owner != galaxy->local.player || !structure.sale_available) return;
            ground_hud.sale_object = structure.id; ground_hud.sale_planet = selected_planet;
            ground_hud.sale_centre = point;
            const auto value = sale_caption(*this, structure);
            ground_hud.sale_label->set_text(String::utf8(value.c_str()));
            layout_sale(*this, true);
            ground_hud.sale->set_disabled(!structure.sale_allowed); ground_hud.sale->show();
            ++ground_hud.sale_opened; return;
        }
        ground_hud.sale_object = 0;
        if (ground_hud.sale) ground_hud.sale->hide();
        if (kind == 3) return;
    }
    if (kind == 0) {
        if (map_input.dragged || map_input.box_start) return;
        ground_hud.input->drag_art->cancel();
        ground_hud.pressed = 0; ground_hud.control = control; ground_hud.double_release = twice;
        ground_hud.drag_sound = false;
        for (const auto& widget : ground_hud.widgets) {
            const auto button = ground_hud.buttons.find("Pad_" + (widget.id < 10 ? std::string("0") : std::string{}) + std::to_string(widget.id));
            if (button != ground_hud.buttons.end() && button->second->get_global_rect().has_point(point)
                && widget.enabled && widget.object && widget.visible) { ground_hud.pressed = widget.id + 1; ground_hud.pressed_object = widget.object; break; }
        }
        const auto found = ground_hud.rosters.find(selected_planet);
        if (control && ground_hud.pressed) {
            const auto widget = std::find_if(ground_hud.widgets.begin(),ground_hud.widgets.end(),
                [this](const auto& row) { return row.id + 1 == ground_hud.pressed; });
            // WGUI-54: stacked input requires the component's explicit flag.
            if (widget == ground_hud.widgets.end() || !widget->stacked_drag) { ground_hud.pressed = 0; return; }
        }
        // Debug build: a press on an enabled, filled pad takes that company for dragging; any other press
        // on the pads (an empty, foreign or disabled one) begins the selection rectangle.
        bool over_pad{};
        for (const auto& [name, button] : ground_hud.buttons)
            over_pad = over_pad || (name.starts_with("Pad_") && button->is_visible_in_tree() && button->get_global_rect().has_point(point));
        if (found != ground_hud.rosters.end())
            static_cast<void>(map_input.begin_box({point.x,point.y}, found->second.allied, ground_hud.pressed != 0 || over_pad));
    } else if (kind == 1) {
        map_input.move_box({point.x,point.y});
        // WGP-184: the held company has left its rest point (GUI_Drag_Fleet, once per press).
        if (ground_hud.pressed_object && map_input.box_end && !ground_hud.drag_sound) {
            if (audio) audio->ui_sound("GUI_Drag_Fleet");
            ground_hud.drag_sound = true;
            const auto id = ground_hud.pressed - 1;
            const auto name = "Pad_" + (id < 10 ? std::string("0") : std::string{}) + std::to_string(id);
            const auto found = ground_hud.buttons.find(name);
            const auto* style = command_catalog.find(name);
            if (found != ground_hud.buttons.end() && style && style->flag(data::ui::Field::should_render_at_drag_pos, true)) {
                auto* button = found->second;
                const auto start = *map_input.box_start;
                const Vector2 grab_point(static_cast<float>(start.x), static_cast<float>(start.y));
                auto* moving = EawrFleetDrag::ghost_of(*button, grab_point - button->get_global_position(), {}, false,
                    style->flag(data::ui::Field::drag_back));
                ground_hud.input->drag_art->start(moving);
                ground_hud.input->drag_art->lift_source(*button, {},
                    style->flag(data::ui::Field::should_ghost) && style->flag(data::ui::Field::ghost_base_only));
                // The existing ground drop moves the multiselection with the pressed company.
                for (const auto selected : map_input.multiselection) {
                    if (selected == id) continue;
                    const auto other_name = "Pad_" + (selected < 10 ? std::string("0") : std::string{}) + std::to_string(selected);
                    const auto other = ground_hud.buttons.find(other_name);
                    const auto* other_style = command_catalog.find(other_name);
                    if (other == ground_hud.buttons.end() || !other_style || other->second->is_disabled()
                        || !other_style->flag(data::ui::Field::should_render_at_drag_pos, true)) continue;
                    moving->add_child(EawrFleetDrag::ghost_of(*other->second, grab_point - other->second->get_global_position(), {}, false,
                        other_style->flag(data::ui::Field::drag_back)));
                    ground_hud.input->drag_art->lift_source(*other->second, {},
                        other_style->flag(data::ui::Field::should_ghost) && other_style->flag(data::ui::Field::ghost_base_only));
                }
            }
        }
    } else {
        ground_hud.input->drag_art->cancel();
        ground_hud.drag_sound = false;
        if (ground_hud.double_release) { map_input.box_start.reset(); map_input.box_end.reset(); }
        else if (ground_launch_drop(point) || ground_pad_drop(point)) { map_input.box_start.reset(); map_input.box_end.reset(); }
        else if (map_input.release_box(ground_hud.widgets)) ++ground_hud.boxes;
        else if (ground_hud.pressed) {
            const auto id = ground_hud.pressed - 1;
            const auto widget = std::find_if(ground_hud.widgets.begin(), ground_hud.widgets.end(), [id](const auto& row) { return row.id == id; });
            const auto button = ground_hud.buttons.find("Pad_" + (id < 10 ? std::string("0") : std::string{}) + std::to_string(id));
            if (widget != ground_hud.widgets.end() && widget->enabled && widget->visible && widget->object && widget->object == ground_hud.pressed_object
                && button != ground_hud.buttons.end() && button->second->get_global_rect().has_point(point)) {
                if (!ground_hud.control) map_input.multiselection.clear();
                const auto selected = std::find(map_input.multiselection.begin(), map_input.multiselection.end(), id);
                if (ground_hud.control && selected != map_input.multiselection.end()) map_input.multiselection.erase(selected);
                else if (selected == map_input.multiselection.end()) map_input.multiselection.push_back(id);
                ++ground_hud.clicks;
            }
        }
        ground_hud.pressed = 0; ground_hud.pressed_object = 0; ground_hud.double_release = false;
    }
    for (const auto& widget : ground_hud.widgets) {
        const auto name = "Pad_" + (widget.id < 10 ? std::string("0") : std::string{}) + std::to_string(widget.id);
        if (const auto button = ground_hud.buttons.find(name); button != ground_hud.buttons.end())
            static_cast<EawrHudButton*>(button->second)->set_order_selected(
                std::find(map_input.multiselection.begin(), map_input.multiselection.end(), widget.id) != map_input.multiselection.end());
    }
}

// #3931: launch and land gating reads the truth roster; with the reveal off the draw roster is the truth roster.
const galactic_view::GroundRoster* GalacticMode::State::command_roster(galactic_view::ObjectId planet) const {
    const auto& source = ground_hud.command_rosters.empty() ? ground_hud.rosters : ground_hud.command_rosters;
    const auto found = source.find(planet);
    return found == source.end() ? nullptr : &found->second;
}

bool GalacticMode::State::request_ground_land(bool submit, galactic_view::ObjectId id) {
    if (!id) id = selected_fleet;
    if (!selectable(*this) || !game || !galaxy || !published_snapshot || !id
        || published_snapshot->clock().pause_depth || !selected_planet) return false;
    const auto* found = command_roster(selected_planet);
    if (!found || !found->accessible) return false;
    const auto fleets = published_snapshot->fleets();
    const auto fleet = std::find_if(fleets.begin(), fleets.end(), [this,id](const auto& row) {
        return row.id == id && row.owner == galaxy->local.player && row.orbit == selected_planet && !row.motion.transit;
    });
    if (fleet == fleets.end()) return false;
    if (!submit) return true;
    const auto planets = input_planets();
    const auto command = map_input.command({galactic_view::DropKind::land, id, 0, selected_planet, {}}, planets);
    if (!command) return false;
    game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *command, {}});
    ++submitted_orders; ++ground_hud.land_orders;
    return true;
}

// WGUI-52/54/55 and WGCH-154: a pressed ground-force widget that moved past the rectangle threshold
// and is released over a same-planet orbit fleet card or orbit slot launches that unit and every
// other multiselected unit (the primary drop callback first, then the rest in selection order).
// Retail's widget-drag activation distance stays WGUI-U04; this reuses the rectangle threshold the
// press already crossed. A release elsewhere keeps the ordinary rectangle selection.
bool GalacticMode::State::ground_launch_drop(Vector2 point) {
    if (!ground_hud.pressed || !ground_hud.pressed_object || !map_input.box_end || !selectable(*this) || !game || !galaxy
        || !published_snapshot || !fleet_organize || !fleet_organize->is_visible_in_tree() || !selected_planet
        || published_snapshot->clock().pause_depth) return false;
    const auto* roster = command_roster(selected_planet);
    if (!roster || !roster->accessible || !roster->allied) return false;
    for (const auto& [name, button] : ground_hud.buttons)
        if (name.starts_with("Pad_") && button->is_visible_in_tree() && button->get_global_rect().has_point(point)) return false;
    galactic_view::ObjectId target{};
    std::uint8_t slot{};
    bool over_target{};
    for (std::size_t index = 0; index < fleet_drop_rects.size() && !over_target; ++index) {
        if (!fleet_drop_rects[index].has_point(point)) continue;
        over_target = true;
        const auto* card = shown_panel_cards[index];
        if (!fleet_panels.drop_slot[index]) return false; // WGUI-175: the other owner's panel takes no drop.
        if (!card || !card->fleet) { slot = *fleet_panels.drop_slot[index]; continue; }
        const auto fleets = published_snapshot->fleets();
        const auto live = std::find_if(fleets.begin(), fleets.end(), [&](const auto& row) {
            return row.id == *card->fleet && row.owner == galaxy->local.player && row.orbit == selected_planet && !row.motion.transit;
        });
        if (live == fleets.end()) return false;
        target = *card->fleet;
    }
    // WGM-199: releasing a surface unit on another planet launches then moves.
    // HUD surfaces consume their own drops; only a picked world planet enters here.
    std::vector<sim::galactic::PlanetId> path;
    if (!over_target) {
        for (const auto* shell : {command_bar, fleet_organize, advanced_map_shell})
            if (shell && shell->is_visible_in_tree() && shell->hit(point.x, point.y)) return false;
        const auto* destination = picked_planet(point.x, point.y);
        if (!destination || destination->id == selected_planet) return false;
        const auto planets = input_planets();
        auto planned = galactic_view::destination_path(*published_snapshot, galaxy->local.player,
            selected_planet, destination->id, planets);
        if (!planned) return false;
        path = std::move(*planned);
        slot = 2; // WGM-199: surface-to-world submission creates in the third source orbit slot.
    }
    const auto enabled = [&](galactic_view::ObjectId object) {
        return std::any_of(roster->companies.begin(), roster->companies.end(),
            [object](const auto& company) { return company.id == object && company.enabled; });
    };
    if (!enabled(ground_hud.pressed_object)) return false;
    std::vector<galactic_view::ObjectId> units{ground_hud.pressed_object};
    for (const auto index : map_input.multiselection) {
        const auto widget = std::find_if(ground_hud.widgets.begin(), ground_hud.widgets.end(),
            [index](const auto& row) { return row.id == index; });
        if (widget == ground_hud.widgets.end() || !widget->object || !enabled(widget->object)
            || std::find(units.begin(), units.end(), widget->object) != units.end()) continue;
        units.push_back(widget->object);
    }
    const auto planets = input_planets();
    const galactic_view::DropIntent intent{galactic_view::DropKind::launch, units.front(), target, selected_planet, std::move(path), slot, units};
    const auto command = map_input.command(intent, planets);
    if (!command) return false;
    game->submit(platform::LiveGameOrder{sim::SeatId{galaxy->local.seat}, *command, {}});
    ++submitted_orders; ++ground_hud.launch_orders;
    ground_hud.last_launch_units = units; ground_hud.last_launch_target = target; ground_hud.last_launch_slot = slot;
    ground_hud.last_launch_path = intent.path;
    map_input.multiselection.clear();
    return true;
}

// GHUD-S69 / WGUI-54: a pressed company released on a pad of its own planet takes that pad (debug
// build planet receive and slot cleanup); the other multiselected companies follow onto the same
// pad. The drop sound plays whether or not a pad was free. Presentation only: retail also writes the
// slot as a per-object GUI index event, which this viewer keeps as local state (GHUD-S69).
bool GalacticMode::State::ground_pad_drop(Vector2 point) {
    if (!ground_hud.pressed || !ground_hud.pressed_object || !map_input.box_end || !selectable(*this)
        || !fleet_organize || !fleet_organize->is_visible_in_tree() || !selected_planet) return false;
    const auto found = ground_hud.rosters.find(selected_planet);
    if (found == ground_hud.rosters.end() || !found->second.accessible || !found->second.allied) return false;
    const auto& roster = found->second;
    std::optional<std::size_t> target;
    for (const auto& [name, button] : ground_hud.buttons) {
        if (!name.starts_with("Pad_") || !button->is_visible_in_tree() || !button->get_global_rect().has_point(point)) continue;
        std::size_t pad{};
        const auto digits = std::string_view(name).substr(4);
        if (std::from_chars(digits.data(), digits.data() + digits.size(), pad).ec == std::errc{}) target = pad;
        break;
    }
    if (!target) return false;
    auto layout = pad_layout(ground_hud, selected_planet, roster);
    const auto before = layout;
    // Retail refuses the drop while the planet holds as many companies as it has pads.
    if (roster.companies.size() < layout.size() && *target < layout.size()) {
        std::vector<galactic_view::ObjectId> moving{ground_hud.pressed_object};
        std::vector<galactic_view::ObjectId> selected;
        for (const auto id : map_input.multiselection) {
            const auto widget = std::find_if(ground_hud.widgets.begin(), ground_hud.widgets.end(),
                [id](const auto& row) { return row.id == id; });
            if (widget == ground_hud.widgets.end() || !widget->object) continue;
            selected.push_back(widget->object);
            if (widget->object != ground_hud.pressed_object) moving.push_back(widget->object);
        }
        for (const auto object : moving) {
            const auto company = std::find_if(roster.companies.begin(), roster.companies.end(),
                [object](const auto& row) { return row.id == object && row.enabled; });
            if (company == roster.companies.end()) continue;
            galactic_view::move_pad(layout, static_cast<std::size_t>(company - roster.companies.begin()), *target);
        }
        auto& slots = ground_hud.pad_slots[selected_planet];
        slots.clear();
        for (std::size_t pad = 0; pad < layout.size(); ++pad)
            if (layout[pad]) slots[roster.companies[*layout[pad]].id] = pad;
        // The selection follows its companies to their new pads.
        map_input.multiselection.clear();
        for (std::size_t pad = 0; pad < layout.size(); ++pad) {
            const auto object = layout[pad] ? roster.companies[*layout[pad]].id : 0;
            if (pad < ground_hud.widgets.size()) {
                ground_hud.widgets[pad].object = object;
                ground_hud.widgets[pad].enabled = layout[pad] && roster.companies[*layout[pad]].enabled;
            }
            if (object && std::find(selected.begin(), selected.end(), object) != selected.end()) map_input.multiselection.push_back(pad);
        }
        ground_hud.layout_dirty = true;
        if (layout != before) ++ground_hud.reorders;
    }
    if (audio) audio->ui_sound("GUI_Drop_Fleet");
    return true;
}

void GalacticMode::State::update_ground_hud() {
    if (!ground_hud.input || !displayed_snapshot || !galaxy) return;
    if (!map_input.box_end || !selectable(*this) || ground_hud.planet != selected_planet)
        ground_hud.input->drag_art->cancel();
    const bool changed = ground_hud.publication != displayed_snapshot || ground_hud.planet != selected_planet
        || ground_hud.layout_dirty;
    ground_hud.layout_dirty = false;
    if (ground_hud.publication != displayed_snapshot) {
        ground_hud.publication = displayed_snapshot; ground_hud.rosters.clear(); ground_hud.command_rosters.clear();
        for (const auto& planet : displayed_snapshot->planets())
            ground_hud.rosters.emplace(planet.id, galactic_view::ground_roster(*displayed_snapshot, galaxy->local.player, planet.id));
        if (published_snapshot && published_snapshot != displayed_snapshot)
            for (const auto& planet : published_snapshot->planets())
                ground_hud.command_rosters.emplace(planet.id,
                    galactic_view::ground_roster(*published_snapshot, galaxy->local.player, planet.id));
        ++ground_hud.rebuilds;
    }
    if (changed) {
        // Slot indices must never retain a selected identity across publication changes.
        if (ground_hud.planet != selected_planet) {
            map_input.multiselection.clear(); map_input.box_start.reset(); map_input.box_end.reset(); ground_hud.pressed = 0;
        } else {
            const auto roster = ground_hud.rosters.find(selected_planet);
            const auto layout = roster == ground_hud.rosters.end() ? galactic_view::PadLayout{}
                : pad_layout(ground_hud, selected_planet, roster->second);
            std::erase_if(map_input.multiselection, [&](auto id) {
                const auto old = std::find_if(ground_hud.widgets.begin(), ground_hud.widgets.end(), [id](const auto& row) { return row.id == id; });
                if (old == ground_hud.widgets.end() || roster == ground_hud.rosters.end() || id >= layout.size() || !layout[id]) return true;
                const auto& company = roster->second.companies[*layout[id]];
                return company.id != old->object || !company.enabled;
            });
        }
        ground_hud.planet = selected_planet;
        ground_hud.widgets.clear();
    }
    const auto found = ground_hud.rosters.find(selected_planet);
    const galactic_view::GroundRoster empty;
    const auto& roster = found == ground_hud.rosters.end() ? empty : found->second;
    if (ground_hud.sale && ground_hud.sale_object) {
        const auto sale = published_snapshot && published_snapshot->production() && galaxy
            ? sim::galactic::object_sale(*published_snapshot->production(), published_snapshot->sale_types(),
                published_snapshot->actor_records(), published_snapshot->fleets(), published_snapshot->control(),
                published_snapshot->movement(), galaxy->local.player, selected_planet, ground_hud.sale_object)
            : sim::galactic::ObjectSale{};
        const bool available = sale.admission == sim::galactic::ProductionAdmission::allowed
            || sale.admission == sim::galactic::ProductionAdmission::locked;
        if (ground_hud.sale_planet != selected_planet || !available
            || !fleet_organize->is_visible_in_tree() || !selectable(*this)) {
            ground_hud.sale_object = 0; ground_hud.sale->hide();
        } else {
            const galactic_view::GroundStructure caption{ground_hud.sale_object, sale.type, galaxy->local.player,
                sale.admission == sim::galactic::ProductionAdmission::allowed, true, sale.refund};
            ground_hud.sale->set_disabled(!caption.sale_allowed);
            const auto value = sale_caption(*this, caption);
            ground_hud.sale_label->set_text(String::utf8(value.c_str()));
            layout_sale(*this);
        }
    }
    const auto placement = fleet_organize->placement();
    const auto layout = pad_layout(ground_hud, selected_planet, roster);
    std::size_t index{};
    for (const auto& [name, native] : ground_hud.buttons) {
        auto* button = static_cast<EawrHudButton*>(native);
        const auto* anchor = fleet_anchors.find(name);
        const auto rect = presentation::ui::shell_to_screen(anchor->rect, placement);
        button->set_position({static_cast<float>(rect.x),static_cast<float>(rect.y)});
        button->set_size({static_cast<float>(rect.width),static_cast<float>(rect.height)});
        button->set_hit_rect({{},button->get_size()});
        if (name.starts_with("Pad_")) {
            const auto* company = index < layout.size() && layout[index] ? &roster.companies[*layout[index]] : nullptr;
            button->set_visible(roster.accessible);
            button->set_disabled(!selectable(*this) || !company || !company->enabled);
            if (changed) {
                const auto* component = command_catalog.find(name);
                auto icon = texture(*this,*component,data::ui::Field::icon_texture_name);
                if (company) {
                    const auto type = production_names.find(company->type);
                    if (type != production_names.end()) {
                        const auto art = production_art.find(type->second);
                        if (art != production_art.end()) icon = command_texture(art->second.icon);
                    }
                }
                button->set_icon_texture(icon);
                button->set_order_selected(std::find(map_input.multiselection.begin(),map_input.multiselection.end(),index) != map_input.multiselection.end());
                ground_hud.widgets.push_back({index,company ? company->id : 0,selected_planet,0,0,true,
                    component->flag(data::ui::Field::drag_select),true,company && company->enabled,
                    component->flag(data::ui::Field::can_drag_stack)});
            }
            if (index < ground_hud.widgets.size()) {
                auto& widget = ground_hud.widgets[index];
                widget.x = button->get_global_position().x; widget.y = button->get_global_position().y;
                widget.visible = button->is_visible_in_tree(); widget.enabled = !button->is_disabled();
            }
            ++index;
        } else if (std::size_t slot{}; structure_slot(name, slot)) {
            // GHUD-S68: slots up to the planet's capacity show, filled ones with the structure icon and
            // enabled, empty ones disabled; unknown contents dim the empty slots.
            const auto* structure = slot < roster.structures.size() ? &roster.structures[slot] : nullptr;
            button->set_visible(slot < std::max(roster.structure_capacity, roster.structures.size()));
            button->set_disabled(!structure);
            if (changed) {
                Ref<Texture2D> icon;
                if (structure) {
                    const auto type = production_names.find(structure->type);
                    if (type != production_names.end()) {
                        const auto art = production_art.find(type->second);
                        if (art != production_art.end()) icon = command_texture(art->second.icon);
                    }
                }
                button->set_icon_texture(icon);
            }
            button->set_modulate(Color(1, 1, 1, structure || roster.structures_known ? 1.0F : 128.0F / 255.0F));
        } else {
            button->set_visible(roster.accessible);
            button->set_disabled(name != "Slot_00" || !request_ground_land(false));
            // GHUD-S85: ordinary focused refresh clears the smuggler drop box's placeholder portrait.
            if (name == "b_underground") button->set_icon_texture({});
        }
    }
    const auto* count_anchor = fleet_anchors.find("Text_Land_Fleet");
    if (count_anchor) {
        const auto rect = presentation::ui::shell_to_screen(count_anchor->rect, placement);
        ground_hud.count->set_position({static_cast<float>(rect.x),static_cast<float>(rect.y)});
        ground_hud.count->set_size({static_cast<float>(rect.width),static_cast<float>(rect.height)});
    }
    if (changed) ground_hud.count->set_text(roster.count ? String::num_uint64(*roster.count) : String{});
    // H052: show the admitted count at its authored mesh or bone point.
    ground_hud.count->set_visible(count_anchor && roster.accessible && roster.count.has_value());
    if (!fleet_organize->is_visible_in_tree() || !selectable(*this)) {
        map_input.box_start.reset(); map_input.box_end.reset(); ground_hud.pressed = 0;
        ground_hud.input->queue_redraw();
    }
}

void GalacticMode::State::write_ground_report(std::ostream& output) const {
    using galactic_detail::json;
    const auto count_rect = ground_hud.count ? ground_hud.count->get_global_rect() : Rect2{};
    output << "{\"count_visible\":" << (ground_hud.count && ground_hud.count->is_visible_in_tree() ? "true" : "false")
        << ",\"count\":" << json(ground_hud.count ? galactic_detail::utf8(ground_hud.count->get_text()) : std::string{})
        << ",\"count_rect\":[" << count_rect.position.x << ',' << count_rect.position.y << ',' << count_rect.size.x << ',' << count_rect.size.y << ']'
        << ",\"projection_rebuilds\":" << ground_hud.rebuilds << ",\"clicks\":" << ground_hud.clicks
        << ",\"sale_opened\":" << ground_hud.sale_opened << ",\"sales_submitted\":" << ground_hud.sales_submitted
        << ",\"sale\":{\"object\":" << ground_hud.sale_object << ",\"visible\":"
        << (ground_hud.sale && ground_hud.sale->is_visible_in_tree() ? "true" : "false")
        << ",\"enabled\":" << (ground_hud.sale && !ground_hud.sale->is_disabled() ? "true" : "false")
        << ",\"text\":" << json(ground_hud.sale_label ? std::string(ground_hud.sale_label->get_text().utf8().get_data()) : std::string{});
    const auto sale_rect = ground_hud.sale ? ground_hud.sale->get_global_rect() : Rect2{};
    output << ",\"rect\":[" << sale_rect.position.x << ',' << sale_rect.position.y << ',' << sale_rect.size.x << ',' << sale_rect.size.y << "]}"
        << ",\"boxes\":" << ground_hud.boxes << ",\"land_orders\":" << ground_hud.land_orders
        << ",\"launch_orders\":" << ground_hud.launch_orders << ",\"reorders\":" << ground_hud.reorders
        << ",\"launch\":{\"command\":" << json(ground_hud.last_launch_path.empty() ? "launch_from_surface opcode 18" : "launch_fleet opcode 22")
        << ",\"target\":" << ground_hud.last_launch_target << ",\"slot\":" << static_cast<unsigned>(ground_hud.last_launch_slot)
        << ",\"units\":[";
    for (std::size_t i = 0; i < ground_hud.last_launch_units.size(); ++i)
        output << (i ? "," : "") << ground_hud.last_launch_units[i];
    output << "],\"path\":[";
    for (std::size_t i = 0; i < ground_hud.last_launch_path.size(); ++i)
        output << (i ? "," : "") << ground_hud.last_launch_path[i];
    output << "]},\"raid\":\"provider seam #2263/#1156\",\"controls\":[";
    std::size_t n{};
    for (const auto& [name,button] : ground_hud.buttons) {
        const auto rect = button->get_global_rect();
        output << (n++ ? "," : "") << "{\"name\":" << json(name) << ",\"visible\":" << (button->is_visible_in_tree() ? "true" : "false")
            << ",\"icon_shown\":" << (static_cast<EawrHudButton*>(button)->icon_shown() ? "true" : "false")
            << ",\"enabled\":" << (!button->is_disabled() ? "true" : "false") << ",\"rect\":[" << rect.position.x << "," << rect.position.y << "," << rect.size.x << "," << rect.size.y << "]}";
    }
    output << "],\"widgets\":["; n = 0;
    for (const auto& widget : ground_hud.widgets)
        output << (n++ ? "," : "") << "{\"slot\":" << widget.id << ",\"object\":" << widget.object
            << ",\"enabled\":" << (widget.enabled ? "true" : "false") << ",\"selected\":"
            << (std::find(map_input.multiselection.begin(),map_input.multiselection.end(),widget.id) != map_input.multiselection.end() ? "true" : "false") << "}";
    output << "],\"structures\":{";
    const auto roster = ground_hud.rosters.find(selected_planet);
    const bool have = roster != ground_hud.rosters.end();
    output << "\"capacity\":" << (have ? roster->second.structure_capacity : 0)
        << ",\"known\":" << (have && roster->second.structures_known ? "true" : "false") << ",\"slots\":[";
    n = 0;
    for (const auto& [name, button] : ground_hud.buttons) {
        std::size_t slot{};
        if (!structure_slot(name, slot)) continue;
        const auto* row = have && slot < roster->second.structures.size() ? &roster->second.structures[slot] : nullptr;
        output << (n++ ? "," : "") << "{\"slot\":" << slot << ",\"object\":" << (row ? row->id : 0)
            << ",\"type\":" << (row ? row->type : 0) << ",\"owner\":" << (row ? row->owner : 0)
            << ",\"visible\":" << (button->is_visible_in_tree() ? "true" : "false")
            << ",\"enabled\":" << (!button->is_disabled() ? "true" : "false")
            << ",\"alpha\":" << button->get_modulate().a << "}";
    }
    output << "]}}";
}
} // namespace eawr::presentation::godot_backend
