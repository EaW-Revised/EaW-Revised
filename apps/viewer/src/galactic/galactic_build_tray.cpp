#include "galactic_build_tray.hpp"
#include "galactic_mode_internal.hpp"
#include "command_bar_scale.hpp"
#include "../settings_store.hpp"
#include "eawr/core/text.hpp"
#include "eawr/presentation/ui/build_tray_geometry.hpp"
#include "eawr/presentation/ui/project_hud.hpp"
#include "ui/hud_icon_text.hpp"
#include "ui/tactical_hud_internal.hpp"

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <sstream>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace tray = presentation::ui::build_tray;
namespace {

Rect2 box_rect(const tray::Box& box) { return Rect2(box.x, box.y, box.width, box.height); }

void set_text_if_changed(EawrHudText& label, const String& value) {
    if (label.get_text() != value) label.set_text(value);
}

} // namespace

// The tray's own layer (#8029): the planet menu (a second HUD shell) sits under an input mask of its own that is a LATER
// child of the HUD than the command bar's mask, and Godot picks a pointer target subtree by subtree in tree order, so a
// tray inside the command bar sat below the planet menu's buttons: they hovered, opened their popup and took clicks
// through the tray. The layer is a child of the HUD placed right after the last of the two shell masks: over every HUD
// shell, and under what the HUD adds after them (the modal menus). The encyclopedia and queue popups are built before the
// shell masks, so in pointer order they have always been under the shells (they draw over them by z, and follow the
// pointer); the layer keeps the tray's place relative to them. The tray's controls keep the command bar's coordinates: the
// layer takes its position and size.
Control* GalacticBuildTray::tray_layer(GalacticMode::State& state) {
    Node* parent = state.hud;
    if (!layer_) {
        layer_ = memnew(Control);
        layer_->set_name("GalacticBuildTrayLayer");
        layer_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        layer_->set_z_index(1); // the shells' masks draw at z 1 of the HUD; a later sibling at that z draws over them
        parent->add_child(layer_);
    }
    int last_shell = -1;
    for (const Control* shell : {static_cast<const Control*>(state.command_bar), static_cast<const Control*>(state.fleet_organize)})
        if (const Node* mask = shell ? shell->get_parent() : nullptr; mask && mask->get_parent() == parent)
            last_shell = std::max(last_shell, mask->get_index());
    if (last_shell >= 0) {
        // Removing the layer first shifts every later child down by one: a layer before the shell lands ON its index.
        const int target = layer_->get_index() < last_shell ? last_shell : last_shell + 1;
        if (layer_->get_index() != target) parent->move_child(layer_, target);
    }
    if (layer_->get_global_position() != state.command_bar->get_global_position())
        layer_->set_global_position(state.command_bar->get_global_position());
    if (layer_->get_size() != state.command_bar->get_size()) layer_->set_size(state.command_bar->get_size());
    return layer_;
}

GalacticBuildTray::GalacticBuildTray() = default;
GalacticBuildTray::~GalacticBuildTray() { clear_backdrop(); }

void GalacticBuildTray::clear_backdrop() {
    if (RenderingServer* rendering = RenderingServer::get_singleton())
        for (const RID& item : backdrop_items_) rendering->free_rid(item);
    backdrop_items_.clear();
    back_layers_ = frame_layers_ = 0;
}

bool GalacticBuildTray::enabled() const {
    if (!layout_ || !layout_->handle.enabled) return false; // GBT-02: disabling the handle disables the tray
    const auto* saved = ActiveSettings::get();
    static const presentation::ui::GameplaySettings untouched{};
    return presentation::ui::galactic_build_tray_enabled(saved ? saved->gameplay : untouched,
        layout_->enabled_by_default);
}

bool GalacticBuildTray::handle_visible() const { return handle_ && handle_->is_visible(); }

bool GalacticBuildTray::toggle_from_key(const std::int64_t keycode) {
    if (!handle_ || !handle_->is_visible_in_tree()) return false;
    const auto* saved = ActiveSettings::get();
    static const presentation::ui::PlayerSettings defaults{};
    const auto binding = presentation::ui::build_tray_key(saved ? *saved : defaults);
    if (bound_keycode_ == 0 || binding != bound_key_) {
        bound_key_ = std::string(binding);
        const auto name = String::utf8(bound_key_.c_str());
        const auto code = OS::get_singleton()->find_keycode_from_string(name);
        // A name the engine spells differently (a hand-edited "b") is no dead binding: it falls back to Tab.
        bound_keycode_ = code != KEY_NONE && OS::get_singleton()->get_keycode_string(code) == name ? code : KEY_TAB;
    }
    if (keycode != bound_keycode_) return false;
    open_ = !open_;
    ++key_presses_;
    return true;
}

const viewer::galactic::BuildChoiceComponent* GalacticBuildTray::choice_under(const Control* control) const {
    if (!control || !open_) return nullptr;
    for (const auto& entry : entries_)
        if (entry.shown && entry.button == control) return &entry.choice;
    return nullptr;
}

void GalacticBuildTray::note_world_event(const Ref<InputEvent>& event) {
    // Every mouse button, wheel and held-button motion event the planet view receives over the tray's controls: the tray
    // takes them all, so the count stays 0 (reported for the GPU case; #8029).
    Vector2 position;
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) position = button->get_position();
    else if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr());
             motion && static_cast<std::int64_t>(motion->get_button_mask()) != 0) position = motion->get_position();
    else return;
    if (holds(position)) ++world_events_over_tray_;
}

bool GalacticBuildTray::holds(const Vector2 point) const {
    for (const Control* control : {static_cast<const Control*>(panel_), static_cast<const Control*>(close_),
             static_cast<const Control*>(handle_)})
        if (control && control->is_visible_in_tree() && control->get_global_rect().has_point(point)) return true;
    return false;
}

void GalacticBuildTray::load_layout(GalacticMode::State& state) {
    // GBT-13: only the layout is read while the switch may be off (its Enabled_By_Default is the switch's default);
    // nothing is reported until the tray is in use.
    if (layout_loaded_ || !state.filesystem) return;
    layout_loaded_ = true;
    const auto layout = tray::load_tray_layout(*state.filesystem);
    layout_ = layout.value;
    if (layout.invalid) layout_error_ = layout.first_error;
}

void GalacticBuildTray::report_layout_error() {
    if (!layout_error_ || layout_error_reported_) return;
    layout_error_reported_ = true;
    UtilityFunctions::push_warning(String::utf8((std::string(layout_
        ? "EAWR-UI-BUILD-TRAY: invalid layout file; retaining the last valid one: "
        : "EAWR-UI-BUILD-TRAY: invalid layout file and no valid one below it; no tray: ") + *layout_error_).c_str()));
}

void GalacticBuildTray::load(GalacticMode::State& state) {
    if (!state.filesystem || state.production_art.empty()) return; // the facts are not ready yet: try again next frame
    if (!loaded_) {
        loaded_ = true;
        report_layout_error();
        const auto groups = tray::load_tray_groups(*state.filesystem);
        if (groups.invalid)
            UtilityFunctions::push_warning(String::utf8(("EAWR-UI-BUILD-TRAY: invalid grouping file; retaining the last valid one: "
                + groups.first_error).c_str()));
        groups_ = groups.value ? *groups.value : tray::TrayGroups{};
        categories_ = tray::load_category_enum(*state.filesystem);
        // GBT-12: a file the mod's category enum contradicts is invalid for that mod; the generic rules apply.
        if (const auto problems = tray::check_against_categories(groups_.definition, categories_); !problems.empty()) {
            UtilityFunctions::push_warning(String::utf8(("EAWR-UI-BUILD-TRAY: the grouping file does not fit this mod: "
                + problems.front()).c_str()));
            groups_ = {};
        }
    }
    if (type_count_ != state.production_art.size()) {
        types_ = viewer::galactic::build_tray_types(state.production_art);
        type_count_ = state.production_art.size();
        // GBT-12: a grouping patch that names types the loaded mod lacks is stale; one note per loaded type set.
        if (const auto stale = tray::stale_assignments(groups_.definition, types_); stale != 0) {
            UtilityFunctions::push_warning(String::utf8(("EAWR-UI-BUILD-TRAY: the grouping file is stale: "
                + std::to_string(stale) + " type assignment(s) name no loaded type").c_str()));
        }
    }
}

void GalacticBuildTray::hide_all() {
    if (handle_) handle_->hide();
    if (panel_) panel_->hide();
    if (close_) close_->hide();
    for (auto& entry : entries_) { entry.shown = false; if (entry.button) entry.button->hide(); }
    for (auto* header : headers_) header->hide();
    for (auto* divider : dividers_) divider->hide();
    handle_docked_ = false;
    omitted_ = 0; groups_shown_ = rows_shown_ = entries_shown_ = 0;
    shown_key_.reset();
    handle_key_.reset();
    handle_choices_.clear();
    bar_.reset();
}

void GalacticBuildTray::ensure_controls(GalacticMode::State& state) {
    if (panel_) return;
    panel_ = memnew(Control);
    panel_->set_name("GalacticBuildTray");
    panel_->set_mouse_filter(Control::MOUSE_FILTER_STOP);
    // A tray control takes the wheel too: Godot passes scroll events through STOP controls by default, and the planet view
    // behind the tray would zoom (#8029, owner: clicks on the tray must not reach the planet view).
    panel_->set_force_pass_scroll_events(false);
    panel_back_ = memnew(TextureRect);
    panel_frame_ = memnew(TextureRect);
    for (auto* image : {panel_back_, panel_frame_}) {
        image->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        image->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        panel_->add_child(image);
    }
    // GBT-10: Back_Component's art (its blank and alternate textures in the mounted command atlas), shown with
    // Back_Source Component, or with Bar when the bar has no art behind its slots. No component is named in code.
    if (const auto* background = layout_->back_component.empty() ? nullptr : state.command_catalog.find(layout_->back_component)) {
        const auto backs = background->list(data::ui::Field::blank_texture_name);
        const auto frames = background->list(data::ui::Field::icon_alternate_texture_name);
        if (!backs.empty()) panel_back_->set_texture(state.command_texture(backs.front()));
        if (!frames.empty()) panel_frame_->set_texture(state.command_texture(frames.front()));
    }
    tray_layer(state)->add_child(panel_);
    panel_->hide();
    // GBT-07: the close icon, in the art of the first Art_Component the mod has (a mod's own small close icon); with
    // none, place_close gives it the bar's own button art, as the handle's.
    const auto& close = layout_->close;
    close_ = memnew(EawrHudButton);
    close_->set_force_pass_scroll_events(false);
    close_->set_name("GalacticBuildTrayClose");
    close_->hide();
    close_->set_action([this] { open_ = false; ++close_presses_; });
    close_text_ = memnew(EawrHudText);
    close_text_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    close_->add_child(close_text_);
    for (const auto& name : close.art_components) {
        const auto* component = state.command_catalog.find(name);
        if (!component) continue;
        const auto first = [&](const data::ui::Field field) {
            const auto list = component->list(field);
            return list.empty() ? Ref<Texture2D>() : state.command_texture_optional(list.front());
        };
        HudButtonArt art{.normal = first(data::ui::Field::icon_texture_name),
            .hover = first(data::ui::Field::mouse_over_texture_name),
            .pressed = first(data::ui::Field::selected_texture_name),
            .disabled = first(data::ui::Field::disabled_texture_name), .portrait = {}, .overlays = {}};
        if (art.normal.is_null()) continue;
        close_->setup_art(art);
        close_->set_sounds(*component, [&state](const std::string& cue) { if (state.audio) state.audio->ui_sound(cue); });
        close_art_ = name;
        break;
    }
    tray_layer(state)->add_child(close_);
    handle_ = memnew(EawrHudButton);
    handle_->set_force_pass_scroll_events(false);
    handle_->set_name("GalacticBuildTrayHandle");
    handle_->hide();
    handle_->set_action([this] { open_ = !open_; ++handle_presses_; });
    handle_text_ = memnew(EawrHudText);
    handle_text_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    handle_->add_child(handle_text_);
    tray_layer(state)->add_child(handle_);
}

void GalacticBuildTray::style_handle(GalacticMode::State& state, const data::ui::CommandBarComponent& component, const data::ui::ShellAnchor& anchor) {
    const auto& definition = layout_->handle;
    const auto placement = state.command_bar->placement();
    const auto texture = [&](const std::string& name) {
        return name.empty() ? Ref<Texture2D>() : state.command_texture_optional(name);
    };
    if (definition.stock_art) {
        // GBT-10: with no texture named the handle uses the bar's own button art, which every mod has.
        const auto pads = component.list(data::ui::Field::blank_texture_name);
        const auto pad = viewer::galactic::build_card_pad(pads, state.selected_hud.space_tab);
        handle_->set_texture_normal(pad.empty() ? Ref<Texture2D>() : state.command_texture(pad));
        handle_->set_texture_hover(state.command_texture(
            [&] { const auto list = component.list(data::ui::Field::mouse_over_texture_name); return list.empty() ? std::string{} : list.front(); }()));
        handle_->set_texture_pressed(state.command_texture(
            [&] { const auto list = component.list(data::ui::Field::selected_texture_name); return list.empty() ? std::string{} : list.front(); }()));
    } else {
        handle_->set_texture_normal(texture(definition.normal));
        handle_->set_texture_hover(texture(definition.hover));
        handle_->set_texture_pressed(texture(definition.pressed));
        handle_->set_texture_disabled(texture(definition.disabled));
    }
    // GBT-02: the caption is the text before the omitted count (the caption alone when the bar omits nothing); a caption
    // key the text database lacks reads as "+".
    std::string caption = state.battle_text ? state.battle_text(definition.caption) : std::string{};
    if (caption.empty() || caption == definition.caption) caption = "+";
    set_text_if_changed(*handle_text_, String::utf8(tray::handle_label(caption, omitted_).c_str()));
    state.configure_command_bar_text(placement, *handle_text_, component, anchor);
    handle_text_->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    handle_text_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    std::string tooltip = state.battle_text ? state.battle_text(definition.tooltip) : std::string{};
    handle_->set_tooltip_text(tooltip.empty() || tooltip == definition.tooltip ? String{} : String::utf8(tooltip.c_str()));
}

void GalacticBuildTray::place_handle(GalacticMode::State& state, const Rect2& bar) {
    const auto& definition = layout_->handle;
    const auto placement = state.command_bar->placement();
    const float scale = static_cast<float>(placement.scale);
    presentation::ui::ProjectHudControl control;
    control.enabled = definition.enabled;
    control.width = definition.width; control.height = definition.height;
    control.stock_art = definition.stock_art;
    for (const auto& candidate : definition.candidates) control.candidates.push_back({candidate.anchor, candidate.side, candidate.x, candidate.y});
    std::map<std::string, presentation::ui::PixelRect> anchors;
    anchors.emplace("production_bar", presentation::ui::PixelRect{bar.position.x, bar.position.y, bar.size.x, bar.size.y});
    for (const auto& [name, button] : state.command_buttons) {
        if (!button || !button->is_visible()) continue;
        anchors.emplace(name, presentation::ui::PixelRect{button->get_position().x, button->get_position().y,
            button->get_size().x, button->get_size().y});
    }
    const auto view = state.hud->get_size();
    const auto docked = presentation::ui::dock_project_hud(control, {0, 0, view.x, view.y}, scale, anchors, {});
    if (!docked) {
        if (!dock_warned_) {
            UtilityFunctions::push_warning("EAWR-UI-BUILD-TRAY: the handle has no candidate position that fits the screen");
            dock_warned_ = true;
        }
        handle_docked_ = false;
        handle_->hide();
        return;
    }
    handle_docked_ = true;
    handle_rect_ = Rect2(static_cast<float>(docked->x), static_cast<float>(docked->y),
        static_cast<float>(docked->width), static_cast<float>(docked->height));
    handle_->set_position(handle_rect_.position);
    handle_->set_size(handle_rect_.size);
    handle_->set_hit_rect(Rect2(Vector2(), handle_rect_.size));
    handle_->show();
}

void GalacticBuildTray::style_entry(GalacticMode::State& state, Entry& entry,
    const viewer::galactic::BuildChoiceComponent& choice, const Rect2& rect, const float entry_scale) {
    const auto placement = state.command_bar->placement();
    const auto* component = state.command_catalog.find(layout_->entry_template);
    const auto* anchor = state.command_anchors.find(layout_->entry_template);
    entry.choice = choice;
    entry.shown = true;
    const auto pads = component->list(data::ui::Field::blank_texture_name);
    const auto dark = component->list(data::ui::Field::disabled_texture_name);
    const auto pad = viewer::galactic::build_card_pad(pads, state.selected_hud.space_tab);
    const auto disabled_pad = viewer::galactic::build_card_pad(dark, state.selected_hud.space_tab);
    entry.button->set_texture_normal(pad.empty() ? Ref<Texture2D>() : state.command_texture(pad));
    entry.button->set_texture_disabled(disabled_pad.empty() ? Ref<Texture2D>() : state.command_texture(disabled_pad));
    const auto icon = state.command_texture(choice.icon);
    entry.button->set_icon_texture(icon);
    entry.button->set_disabled(!choice.enabled);
    entry.button->set_tooltip_text(String{}); // the shared timed encyclopedia owns object hover
    // GBT-06: the entry is the bar's card at the bar's size, scaled whole by Entry_Scale: icon, price, badges and the
    // hit area shrink together, so the price never crowds a smaller icon.
    const Vector2 card = rect.size / std::max(entry_scale, 1e-3F);
    // GHUD-S74 (#7956): inside the card the icon is drawn as on the bar, at its own size times the template's Scale,
    // centred, aspect kept; Entry_Scale then shrinks the whole card.
    if (icon.is_valid()) {
        const auto box = viewer::galactic::command_layer_rect(card.x, card.y, static_cast<float>(icon->get_width()),
            static_cast<float>(icon->get_height()),
            viewer::galactic::command_bar_component_scale(*component, static_cast<float>(placement.scale)));
        entry.button->set_cell_portrait_box(Rect2(box.x, box.y, box.width, box.height));
    }
    entry.button->set_position(rect.position);
    entry.button->set_size(card);
    entry.button->set_scale(Vector2(entry_scale, entry_scale));
    entry.button->set_hit_rect(Rect2(Vector2(), card));
    entry.button->show();
    // GHUD-S57: the card's price, as on the bar, in the price state's colour.
    set_text_if_changed(*entry.price, choice.price ? String::num_int64(choice.price->trunc_to_integer()) : String{});
    const auto offset = component->vec2(data::ui::Field::text_offset2).value_or(data::ui::Vec2{});
    state.configure_command_bar_text(placement, *entry.price, *component, *anchor);
    const auto& tint = choice.price_colour;
    entry.price->set_colour(Color(tint[0] / 255.0F, tint[1] / 255.0F, tint[2] / 255.0F, tint[3] / 255.0F));
    entry.price->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    const float unit = static_cast<float>(placement.scale); // the button's own scale shrinks the offset with it
    entry.price->set_anchors_and_offsets_preset(Control::PRESET_BOTTOM_WIDE);
    entry.price->set_offset(SIDE_TOP, offset.y * unit);
    entry.price->set_offset(SIDE_BOTTOM, 0.0F);
    entry.price->set_offset(SIDE_LEFT, offset.x * unit);
    entry.price->set_offset(SIDE_RIGHT, offset.x * unit);
}

float GalacticBuildTray::panel_width(GalacticMode::State& state, const Rect2& bar) const {
    const auto& layout = *layout_;
    const float ui = static_cast<float>(state.command_bar->placement().scale);
    // GBT-06: never wider than Max_Width_Share of the view, never past the view's right edge, and never into the planet
    // info card (#7952), in every width mode.
    const float view = state.hud->get_size().x;
    const float widest = tray::widest_panel(view, bar.position.x, static_cast<float>(layout.max_width_share),
        state.planet_info_card_left());
    switch (layout.width_mode) {
    case tray::WidthMode::fixed: return std::min(static_cast<float>(layout.width) * ui, widest);
    case tray::WidthMode::bar: return std::min(bar.size.x, widest);
    case tray::WidthMode::automatic: break;
    }
    // Auto (owner decision of 2026-11-19, #7952): the whole span from the tray's left edge to the planet info card, at most
    // Max_Width_Share of the view; never the bar's width, which follows how many slots the bar shows. The rows and groups
    // fold inside it (GBT-06).
    return widest;
}

void GalacticBuildTray::draw_backdrop(GalacticMode::State& state, const Rect2& bar) {
    clear_backdrop();
    back_sample_ = Vector2();
    const auto& layout = *layout_;
    const auto shell = state.command_bar->placement();
    if (layout.back_source == tray::BackSource::bar && shell.scale > 0.0) {
        RenderingServer* rendering = RenderingServer::get_singleton();
        const auto to_shell = [&](const Vector2 point) {
            return std::pair{(point.x - shell.left) / shell.scale, (shell.bottom - point.y) / shell.scale};
        };
        const auto& meshes = state.command_bar->meshes();
        const auto alternate = state.command_bar->alternate();
        // One canvas item per shell mesh that holds the centre of `source` (screen pixels): that mesh's art drawn over each
        // of `targets` (panel pixels). The bar's own shell data decides the art and the colour, so a mod that recolours its
        // bar recolours the tray with it. The texture map is the one triangle's under the centre, which holds only over
        // that triangle: the sample is cut to the triangle's box in x and y, repeated across the target and stretched
        // vertically, so no art from elsewhere on the texture sheet shows (#7952).
        Vector2 sample; // the last sampled span (screen x from, to), for the report
        const auto layers = [&](const Rect2& source, std::initializer_list<Rect2> targets) {
            std::size_t count{};
            const auto [cx, cy] = to_shell(source.get_center());
            for (const auto& mesh : meshes) {
                if (mesh.texture.is_null() || (mesh.alternate && mesh.alternate != alternate)) continue;
                const auto map = tray::uv_map_at(mesh.triangles, cx, cy);
                if (!map) continue;
                const auto uv = [&](const float x, const float y) {
                    const auto [sx, sy] = to_shell(Vector2(x, y));
                    const auto [u, v] = map->at(sx, sy);
                    return Vector2(static_cast<float>(u), static_cast<float>(v));
                };
                const float left = std::max(source.position.x, static_cast<float>(shell.left + map->x_min * shell.scale));
                const float right = std::min(source.get_end().x, static_cast<float>(shell.left + map->x_max * shell.scale));
                // Shell y grows upward: the triangle's top is its y_max.
                const float top = std::max(source.position.y, static_cast<float>(shell.bottom - map->y_max * shell.scale));
                const float bottom = std::min(source.get_end().y, static_cast<float>(shell.bottom - map->y_min * shell.scale));
                if (!(right > left) || !(bottom > top)) continue;
                sample = Vector2(left, right);
                PackedVector2Array points, uvs;
                PackedColorArray colours;
                PackedInt32Array indices;
                for (const Rect2& target : targets) {
                    if (target.size.x <= 0.0F || target.size.y <= 0.0F) continue;
                    const float target_bottom = target.get_end().y;
                    for (const auto& piece : tray::band_pieces(target.position.x, target.size.x, right - left)) {
                        const auto first = static_cast<int32_t>(points.size());
                        const float piece_right = piece.x + piece.width;
                        for (const Vector2 point : {Vector2(piece.x, target.position.y), Vector2(piece_right, target.position.y),
                                 Vector2(piece_right, target_bottom), Vector2(piece.x, target_bottom)})
                            points.push_back(point);
                        const float sample_right = left + piece.width;
                        for (const Vector2 corner : {uv(left, top), uv(sample_right, top), uv(sample_right, bottom), uv(left, bottom)}) {
                            uvs.push_back(corner);
                            colours.push_back(Color(1, 1, 1, 1));
                        }
                        for (const int32_t index : {0, 1, 2, 0, 2, 3}) indices.push_back(first + index);
                    }
                }
                if (points.is_empty()) continue;
                const RID item = rendering->canvas_item_create();
                rendering->canvas_item_set_parent(item, panel_->get_canvas_item());
                // As the bar draws its shell: repeating texture coordinates, sampled linearly, additive meshes added.
                rendering->canvas_item_set_default_texture_repeat(item, RenderingServer::CANVAS_ITEM_TEXTURE_REPEAT_ENABLED);
                rendering->canvas_item_set_default_texture_filter(item, RenderingServer::CANVAS_ITEM_TEXTURE_FILTER_LINEAR);
                if (mesh.blend == presentation::ui::ShellBlend::additive) {
                    if (additive_.is_null()) {
                        additive_.instantiate();
                        additive_->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
                    }
                    rendering->canvas_item_set_material(item, additive_->get_rid());
                }
                rendering->canvas_item_add_triangle_array(item, indices, points, colours, uvs, PackedInt32Array(),
                    PackedFloat32Array(), mesh.texture->get_rid());
                backdrop_items_.push_back(item);
                ++count;
            }
            return count;
        };
        const Vector2 size = panel_rect_.size;
        // The back: the bar's art behind its slots, over the whole panel.
        back_layers_ = layers(bar, {Rect2(Vector2(), size)});
        back_sample_ = sample;
        // The frame: the bar's art in the band just above its slots (its top edge), around the panel.
        const float frame = std::min({static_cast<float>(layout.frame_width * shell.scale), size.x * 0.5F, size.y * 0.5F});
        if (back_layers_ != 0 && frame > 0.0F) {
            frame_layers_ = layers(Rect2(bar.position.x, bar.position.y - frame, bar.size.x, frame),
                {Rect2(0, 0, size.x, frame), Rect2(0, size.y - frame, size.x, frame),
                 Rect2(0, frame, frame, size.y - 2.0F * frame), Rect2(size.x - frame, frame, frame, size.y - 2.0F * frame)});
        }
    }
    const bool bar_art = back_layers_ != 0;
    const bool component_art = !bar_art && (panel_back_->get_texture().is_valid() || panel_frame_->get_texture().is_valid());
    panel_back_->set_visible(component_art);
    panel_frame_->set_visible(component_art);
    const std::string source = bar_art ? "bar" : component_art ? "component" : "none";
    if (source == "none" && backdrop_source_ != "none")
        UtilityFunctions::push_warning("EAWR-UI-BUILD-TRAY: the bar has no shell art behind its slots and no Back_Component art is"
            " in this mod; the tray panel has no back");
    backdrop_source_ = source;
}

void GalacticBuildTray::place_close(GalacticMode::State& state) {
    const auto& definition = layout_->close;
    if (!definition.enabled) { close_->hide(); return; }
    const auto placement = state.command_bar->placement();
    if (close_art_.empty()) {
        // The bar's own button art, as the handle's stock art: the entry template's pad for the shown tab.
        if (const auto* component = state.command_catalog.find(layout_->entry_template)) {
            const auto first = [&](const data::ui::Field field) {
                const auto list = component->list(field);
                return list.empty() ? Ref<Texture2D>() : state.command_texture_optional(list.front());
            };
            const auto pad = viewer::galactic::build_card_pad(component->list(data::ui::Field::blank_texture_name),
                state.selected_hud.space_tab);
            close_->set_texture_normal(pad.empty() ? Ref<Texture2D>() : state.command_texture(pad));
            close_->set_texture_hover(first(data::ui::Field::mouse_over_texture_name));
            close_->set_texture_pressed(first(data::ui::Field::selected_texture_name));
            close_->set_sounds(*component, [&state](const std::string& cue) { if (state.audio) state.audio->ui_sound(cue); });
        }
    }
    presentation::ui::ProjectHudControl control;
    control.enabled = true;
    control.width = definition.width; control.height = definition.height;
    for (const auto& candidate : definition.candidates) control.candidates.push_back({candidate.anchor, candidate.side, candidate.x, candidate.y});
    std::map<std::string, presentation::ui::PixelRect> anchors;
    anchors.emplace("build_tray", presentation::ui::PixelRect{panel_rect_.position.x, panel_rect_.position.y, panel_rect_.size.x, panel_rect_.size.y});
    if (bar_) anchors.emplace("production_bar", presentation::ui::PixelRect{bar_->position.x, bar_->position.y, bar_->size.x, bar_->size.y});
    for (const auto& [name, button] : state.command_buttons) {
        if (!button || !button->is_visible()) continue;
        anchors.emplace(name, presentation::ui::PixelRect{button->get_position().x, button->get_position().y,
            button->get_size().x, button->get_size().y});
    }
    const auto view = state.hud->get_size();
    const auto docked = presentation::ui::dock_project_hud(control, {0, 0, view.x, view.y}, placement.scale, anchors, {});
    if (!docked) { close_->hide(); return; }
    close_rect_ = Rect2(static_cast<float>(docked->x), static_cast<float>(docked->y),
        static_cast<float>(docked->width), static_cast<float>(docked->height));
    close_->set_position(close_rect_.position);
    close_->set_size(close_rect_.size);
    close_->set_hit_rect(Rect2(Vector2(), close_rect_.size));
    // Its label and tooltip are text-database keys; one the database lacks shows nothing rather than the key.
    const auto text = [&](const std::string& key) {
        std::string value = state.battle_text && !key.empty() ? state.battle_text(key) : std::string{};
        return value.empty() || value == key ? String{} : String::utf8(value.c_str());
    };
    // As the handle's "+": a caption key the text database lacks reads as the built-in x.
    const String caption = text(definition.caption);
    set_text_if_changed(*close_text_, caption.is_empty() && !definition.caption.empty() && close_art_.empty() ? String("x") : caption);
    // The label takes the headers' text look (Header_Template, else the entry template).
    const auto* component = state.command_catalog.find(layout_->header_template);
    const auto* anchor = state.command_anchors.find(layout_->header_template);
    if (!component || !anchor) {
        component = state.command_catalog.find(layout_->entry_template);
        anchor = state.command_anchors.find(layout_->entry_template);
    }
    if (component && anchor) state.configure_command_bar_text(placement, *close_text_, *component, *anchor);
    close_text_->set_horizontal_alignment(HORIZONTAL_ALIGNMENT_CENTER);
    close_text_->set_anchors_and_offsets_preset(Control::PRESET_FULL_RECT);
    close_->set_tooltip_text(text(definition.tooltip));
    close_->show();
    close_->move_to_front(); // over the entries when a candidate docks it inside the panel
}

void GalacticBuildTray::layout_tray(GalacticMode::State& state, const Rect2& bar) {
    const auto& layout = *layout_;
    const auto& choices = state.selected_hud.choices;
    const auto placement = state.command_bar->placement();
    const float ui = static_cast<float>(placement.scale);
    const auto* template_button = state.command_buttons.at(layout.entry_template);
    const auto* component = state.command_catalog.find(layout.entry_template);
    const auto* anchor = state.command_anchors.find(layout.entry_template);
    if (!component || !anchor || !template_button) { panel_->hide(); return; }
    const Vector2 base = template_button->get_size();

    // Nothing changed since the last layout: the controls already show it.
    const LayoutKey key{state.selected_planet, state.selected_hud.space_tab, ui, state.hud->get_size(), bar, handle_rect_};
    if (shown_key_ && *shown_key_ == key && shown_choices_ == choices && panel_->is_visible()) return;
    shown_key_ = key;
    shown_choices_ = choices;

    // Metrics at the full and at the compact size step (GBT-06).
    const auto metrics_for = [&](const float step) {
        tray::Metrics metrics;
        metrics.entry_width = base.x * step;
        metrics.entry_height = base.y * step;
        metrics.entry_gap = static_cast<float>(layout.entry_gap) * ui;
        metrics.group_gap = static_cast<float>(layout.group_gap) * ui;
        metrics.row_gap = static_cast<float>(layout.row_gap) * ui;
        metrics.padding = static_cast<float>(layout.panel_padding) * ui;
        return metrics; // the panel width is set once the groups are known (Width_Mode Auto depends on them)
    };
    auto full = metrics_for(static_cast<float>(layout.entry_scale));
    auto compact = metrics_for(static_cast<float>(layout.compact_entry_scale));

    // Header text: the Header_Template component's text look (a readable command bar label), or the entry template's
    // when the layout names none or the mod lacks it; scaled by Header_Scale.
    const auto* header_component = layout.header_template.empty() ? nullptr : state.command_catalog.find(layout.header_template);
    const auto* header_anchor = layout.header_template.empty() ? nullptr : state.command_anchors.find(layout.header_template);
    if (!header_component || !header_anchor) { header_component = component; header_anchor = anchor; }
    if (headers_.empty()) {
        auto* label = memnew(EawrHudText);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->hide();
        tray_layer(state)->add_child(label);
        headers_.push_back(label);
    }
    auto* probe = headers_.front();
    const float header_scale = static_cast<float>(layout.header_scale);
    const auto measure = [&](const std::string& text) {
        set_text_if_changed(*probe, String::utf8(text.c_str()));
        state.configure_command_bar_text(placement, *probe, *header_component, *header_anchor);
        // A HUD text's minimum height is 0 (its band is the authored one, hud_icon_text.hpp), so the header strip takes the
        // height of the text itself: the font's line height at the glyph size the label draws with (#8159: a strip of height
        // 0 put every label half over the icons below it).
        const auto font = probe->font();
        // No font yet: the glyph size itself, never 0 (a zero strip is the bug this fixes).
        const float line = font.is_valid() ? font->get_height(probe->glyph_pixels())
                                           : static_cast<float>(std::max(probe->glyph_pixels(), 1));
        return Vector2(probe->get_minimum_size().x, line) * header_scale;
    };
    const float header_height = measure("Ag").y;
    header_line_ = header_height;
    full.header_height = header_height;
    // The label draws the same line at either size step (it is not scaled with the entries), so the compact strip is the
    // full line too: a shorter strip let the text run on over the icons below it.
    compact.header_height = header_height;
    // GBT-06: a header label rides a quarter of its own height above the icons below it, in a gap of its own; both are
    // relative to the header's height, so every skin and mod font gets the same proportion (#8031).
    for (auto* metrics : {&full, &compact}) {
        metrics->header_lift = 0.25F * metrics->header_height;
        metrics->category_gap = 3.0F * metrics->header_lift;
    }

    const auto caption_key = [&](const tray::Group& group) {
        for (const auto& [name, key_text] : groups_.definition.captions)
            if (core::iequals(name, group.name)) return key_text;
        return std::string{};
    };
    const auto caption_text = [&](const tray::Group& group) {
        if (!group.label.empty()) return group.label; // a group split by encyclopedia class: the mod's own class text (GBT-05)
        const auto key_text = !group.caption_key.empty() ? group.caption_key : caption_key(group);
        if (!key_text.empty() && state.battle_text) {
            auto resolved = state.battle_text(key_text);
            if (!resolved.empty() && resolved != key_text) return resolved;
        }
        return tray::derived_caption(group.name);
    };

    // Classify and pack (GBT-04 to GBT-06).
    std::vector<std::string> types;
    types.reserve(choices.size());
    for (const auto& choice : choices) types.push_back(choice.type);
    // The headers of whole category groups, as the labels will read them: the model merges a class group into a category
    // group that reads the same (GBT-05), in tray order.
    std::vector<std::pair<std::string, std::string>> category_headers;
    category_headers.reserve(categories_.names.size());
    for (const auto& name : categories_.names)
        category_headers.emplace_back(name, caption_text(tray::Group{tray::GroupKind::category, name, {}, {}, 0.0F, {}}));
    auto groups = tray::classify(types, types_, categories_, groups_.definition, category_headers);
    for (auto& group : groups) group.min_width = measure(caption_text(group)).x;
    tray::PackOptions options;
    options.max_rows = layout.max_rows;
    options.fold_single_choice_groups = layout.fold_single_choice_groups;
    options.misc_min_width = measure(caption_text(tray::Group{tray::GroupKind::misc, "Misc", {}, {}, 0.0F, {}})).x;
    if (const auto misc_key = caption_key(tray::Group{tray::GroupKind::misc, "Misc", {}, {}, 0.0F, {}}); !misc_key.empty())
        options.misc_caption_key = misc_key;
    // GBT-07: a close icon whose first candidate sits inside the panel's top keeps a strip of its height (plus its
    // offset from the edge) free above the first row, so it never covers a header or an entry.
    if (const auto& close = layout.close; close.enabled && !close.candidates.empty()
        && core::iequals(close.candidates.front().anchor, "build_tray") && close.candidates.front().side.starts_with("inside_top")) {
        const float reserve = std::max(0.0F, static_cast<float>(close.height + close.candidates.front().y) * ui + full.row_gap - full.padding);
        full.top_reserve = compact.top_reserve = reserve;
    }

    // The tray's bottom edge sits on the top of the box around the bar's slots and the handle; its top limit is the
    // layout's (GBT-11): a component's bottom edge, a reference height, or the top of the view.
    float bottom = bar.position.y;
    if (handle_docked_) bottom = std::min(bottom, handle_rect_.position.y);
    float top = 0.0F;
    if (!layout.galaxy_top_component.empty()) {
        if (const auto found = state.command_buttons.find(layout.galaxy_top_component); found != state.command_buttons.end() && found->second->is_visible())
            top = found->second->get_position().y + found->second->get_size().y;
    } else if (layout.galaxy_top_y) {
        top = static_cast<float>(*layout.galaxy_top_y) * ui;
    }
    const float free_height = std::max(0.0F, bottom - top);
    full.panel_width = compact.panel_width = panel_width(state, bar);
    const auto fit = tray::fit_to_height(groups, full, compact, options, free_height);
    compact_ = fit.compact;
    const auto& metrics = fit.compact ? compact : full;
    const auto geometry = tray::place(fit.packed, metrics, bar.position.x, bottom);
    omitted_by_layout_ = fit.packed.omitted;
    // GBT-06: entries the row budget cannot hold are lost from the tray; say so once per planet and count.
    if (omitted_by_layout_ != 0 && lost_reported_ != std::pair{state.selected_planet, omitted_by_layout_}) {
        lost_reported_ = std::pair{state.selected_planet, omitted_by_layout_};
        UtilityFunctions::push_warning(String::utf8(("EAWR-UI-BUILD-TRAY: " + std::to_string(omitted_by_layout_)
            + " build choice(s) do not fit the tray's row budget and are not shown").c_str()));
    }
    groups_shown_ = fit.packed.groups.size();
    rows_shown_ = fit.packed.rows.size();
    entries_shown_ = geometry.entries.size();

    // Panel.
    panel_rect_ = box_rect(geometry.panel);
    panel_->set_position(panel_rect_.position);
    panel_->set_size(panel_rect_.size);
    panel_back_->set_position(Vector2(ui, ui));
    panel_back_->set_size(panel_rect_.size - Vector2(2.0F * ui, 2.0F * ui));
    panel_frame_->set_position(Vector2());
    panel_frame_->set_size(panel_rect_.size);
    draw_backdrop(state, bar);
    panel_->show();
    place_close(state);

    // Entries: one pooled copy of the bar's production button each.
    const float entry_scale = fit.compact ? static_cast<float>(layout.compact_entry_scale) : static_cast<float>(layout.entry_scale);
    while (entries_.size() < geometry.entries.size()) {
        Entry entry;
        entry.button = memnew(EawrHudButton);
        entry.button->set_force_pass_scroll_events(false);
        entry.button->set_name("build_tray_entry");
        entry.button->hide();
        entry.button->set_order_icon({}, component->flag(data::ui::Field::click_shift));
        entry.button->set_texture_hover(state.command_texture(
            [&] { const auto list = component->list(data::ui::Field::mouse_over_texture_name); return list.empty() ? std::string{} : list.front(); }()));
        entry.button->set_texture_pressed(state.command_texture(
            [&] { const auto list = component->list(data::ui::Field::selected_texture_name); return list.empty() ? std::string{} : list.front(); }()));
        entry.button->set_sounds(*component, [&state](const std::string& sfx) { if (state.audio) state.audio->ui_sound(sfx); });
        entry.price = memnew(EawrHudText);
        entry.price->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        entry.price->set_name("build_card_price");
        entry.button->add_child(entry.price);
        const auto index = entries_.size();
        entry.button->set_action([this, index] {
            if (index >= entries_.size() || !entries_[index].shown || !open_ || !state_) return;
            state_->submit_build_choice(entries_[index].choice);
        });
        tray_layer(state)->add_child(entry.button);
        entries_.push_back(entry);
    }
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (index >= geometry.entries.size()) { entries_[index].shown = false; entries_[index].button->hide(); continue; }
        const auto& placed = geometry.entries[index];
        style_entry(state, entries_[index], choices[placed.choice], box_rect(placed.box), entry_scale);
    }

    // Headers.
    while (headers_.size() < geometry.headers.size() + 1) {
        auto* label = memnew(EawrHudText);
        label->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        label->hide();
        tray_layer(state)->add_child(label);
        headers_.push_back(label);
    }
    probe->hide();
    for (std::size_t index = 0; index < headers_.size() - 1; ++index) {
        auto* label = headers_[index + 1];
        if (index >= geometry.headers.size()) { label->hide(); continue; }
        const auto& placed = geometry.headers[index];
        set_text_if_changed(*label, String::utf8(caption_text(fit.packed.groups[placed.group]).c_str()));
        state.configure_command_bar_text(placement, *label, *header_component, *header_anchor);
        label->set_scale(Vector2(header_scale, header_scale));
        label->set_position(Vector2(placed.box.x, placed.box.y));
        label->set_size(Vector2(placed.box.width / header_scale, placed.box.height / header_scale));
        label->show();
    }

    // Dividers: the layout's texture in the command atlas; none named, none drawn.
    const auto divider_texture = layout.divider_texture.empty() ? Ref<Texture2D>() : state.command_texture_optional(layout.divider_texture);
    while (dividers_.size() < geometry.dividers.size()) {
        auto* divider = memnew(TextureRect);
        divider->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        divider->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        divider->hide();
        tray_layer(state)->add_child(divider);
        dividers_.push_back(divider);
    }
    for (std::size_t index = 0; index < dividers_.size(); ++index) {
        auto* divider = dividers_[index];
        if (index >= geometry.dividers.size() || divider_texture.is_null()) { divider->hide(); continue; }
        const auto& placed = geometry.dividers[index];
        const float width = std::max(1.0F, static_cast<float>(divider_texture->get_width()) * ui);
        divider->set_texture(divider_texture);
        divider->set_position(Vector2(placed.x - width * 0.5F, placed.y));
        divider->set_size(Vector2(width, placed.height));
        divider->show();
    }
}

void GalacticBuildTray::service(GalacticMode::State& state) {
    state_ = &state;
    if (!state.command_bar || !state.hud) return;
    update(state);
    if (layer_) static_cast<void>(tray_layer(state)); // every frame: the HUD may have gained a sibling, the bar may have moved
    lift_queue_strips(state);
}

void GalacticBuildTray::lift_queue_strips(GalacticMode::State& state) {
    // GBT-14 (owner pick B): a queue strip the open tray would cover moves up above the tray, and no strip ever covers
    // the handle. Every frame, from each button's place as the bar put it: a closed tray or no handle gives no lift
    // and the strip returns to its place.
    const auto box = [](const Rect2& rect) { return tray::Box{rect.position.x, rect.position.y, rect.size.x, rect.size.y}; };
    std::optional<tray::Box> panel, handle;
    if (panel_ && panel_->is_visible() && open_) panel = box(panel_rect_);
    if (handle_ && handle_->is_visible()) handle = box(handle_rect_);
    // The queue buttons are made once with the bar (State::queue_buttons, land strip first, then space): resolve them
    // once per bar, so a frame does no lookup and no allocation. lifts_ reports [space, land].
    if (lifted_for_ != state.command_bar->get_instance_id()) {
        lifted_.clear();
        for (std::size_t domain = 0; domain < state.queue_buttons.size(); ++domain)
            for (const auto& name : state.queue_buttons[domain])
                if (const auto found = state.command_buttons.find(name); found != state.command_buttons.end() && found->second)
                    lifted_.push_back({found->second, domain, found->second->get_position(), std::nullopt});
        lifted_for_ = state.command_bar->get_instance_id();
    }
    for (std::size_t domain = 0; domain < state.queue_buttons.size(); ++domain) {
        std::optional<Rect2> strip;
        for (auto& memo : lifted_) {
            if (memo.domain != domain) continue;
            const Vector2 now = memo.button->get_position();
            if (!memo.applied || now != *memo.applied) memo.base = now; // the bar placed it anew
            if (!memo.button->is_visible()) continue;
            const Rect2 rect(memo.base, memo.button->get_size());
            strip = strip ? strip->merge(rect) : rect;
        }
        // The strip sits tight on what it clears (no gap); it stays unbacked, its icons floating over the galaxy
        // (owner, 2026-11-19: "like a hologram").
        const float lift = strip ? tray::queue_strip_lift(box(*strip), panel, handle, 0.0F) : 0.0F;
        for (auto& memo : lifted_) {
            if (memo.domain != domain) continue;
            const Vector2 target = memo.base - Vector2(0.0F, lift);
            if (memo.button->get_position() != target) memo.button->set_position(target);
            memo.applied = target;
        }
        lifts_[domain == 0 ? 1 : 0] = lift;
    }
}

void GalacticBuildTray::update(GalacticMode::State& state) {
    load_layout(state);
    if (!layout_ && layout_error_) {
        // No valid layout in any layer: there is no tray, but a player who has the switch on hears why once.
        const auto* saved = ActiveSettings::get();
        static const presentation::ui::GameplaySettings untouched{};
        if (presentation::ui::galactic_build_tray_enabled(saved ? saved->gameplay : untouched, true)) report_layout_error();
    }
    if (!layout_ || !enabled() || state.tactical || state.loading || !state.command_bar->is_visible_in_tree()) { hide_all(); return; }
    load(state);
    if (!loaded_) { hide_all(); return; }
    const auto& choices = state.selected_hud.choices;
    const auto found = state.command_buttons.find(layout_->entry_template);
    // GBT-07: a planet that is not the local player's has no choices, so no handle and no tray; the open state is kept.
    if (choices.empty() || state.choice_buttons.empty() || found == state.command_buttons.end()) { hide_all(); return; }

    // The handle (its count, art, text and place) and the bar box change only with this key and the choice list:
    // an unchanged frame does no slot mapping, text lookup or docking.
    const HandleKey key{state.selected_planet, state.selected_hud.space_tab,
        static_cast<float>(state.command_bar->placement().scale), state.hud->get_size(), state.choice_buttons.size()};
    if (!handle_key_ || *handle_key_ != key || handle_choices_ != choices) {
        hide_all();
        handle_key_ = key;
        handle_choices_ = choices;
        const auto slots = viewer::galactic::production_choice_slots(choices, state.choice_buttons.size());
        omitted_ = tray::omitted_by_bar(choices.size(), slots);
        // GBT-02 (owner, 2026-11-19): the handle is there for every planet with build choices, whether or not the bar
        // omits some; the tray (and Tab) then shows the grouped list of all of them. The handle counts the omitted ones.
        ensure_controls(state);
        // The box around the bar's slots, hidden or not (the width of the bar).
        for (const auto& name : state.choice_buttons) {
            if (name.empty()) continue;
            const auto* button = state.command_buttons.at(name);
            const Rect2 rect(button->get_position(), button->get_size());
            bar_ = bar_ ? bar_->merge(rect) : rect;
        }
        const auto* component = state.command_catalog.find(layout_->entry_template);
        const auto* anchor = state.command_anchors.find(layout_->entry_template);
        if (!bar_ || !component || !anchor) { bar_.reset(); return; }
        style_handle(state, *component, *anchor);
        place_handle(state, *bar_);
    }
    if (!bar_) return;
    if (!handle_docked_ || !open_) {
        if (shown_key_) {
            panel_->hide();
            close_->hide();
            for (auto& entry : entries_) { entry.shown = false; entry.button->hide(); }
            for (auto* header : headers_) header->hide();
            for (auto* divider : dividers_) divider->hide();
            shown_key_.reset();
        }
        return;
    }
    layout_tray(state, *bar_);
}

std::string GalacticBuildTray::report_json() const {
    std::ostringstream out;
    const auto escaped = [](const std::string& text) {
        std::string result;
        for (const char c : text) { if (c == '"' || c == '\\') result += '\\'; result += c; }
        return result;
    };
    out << "{\"enabled\":" << (layout_ && enabled() ? "true" : "false")
        << ",\"handle\":" << (handle_visible() ? "true" : "false")
        << ",\"handle_text\":\"" << escaped(handle_visible() && handle_text_ ? std::string(handle_text_->get_text().utf8().get_data()) : std::string{}) << '"'
        << ",\"open\":" << (open_ ? "true" : "false")
        << ",\"omitted\":" << omitted_
        << ",\"groups\":" << groups_shown_
        << ",\"rows\":" << rows_shown_
        << ",\"entries\":" << entries_shown_
        << ",\"lost\":" << omitted_by_layout_
        << ",\"compact\":" << (compact_ ? "true" : "false")
        << ",\"handle_presses\":" << handle_presses_
        << ",\"queue_lift\":[" << lifts_[0] << ',' << lifts_[1] << ']'
        << ",\"key_presses\":" << key_presses_
        << ",\"world_events_over_tray\":" << world_events_over_tray_;
    const auto rect_json = [&out](const Rect2& rect) {
        out << '[' << rect.position.x << ',' << rect.position.y << ',' << rect.size.x << ',' << rect.size.y << ']';
    };
    out << ",\"handle_rect\":";
    rect_json(handle_visible() ? handle_->get_global_rect() : Rect2());
    const bool panel_shown = panel_ && panel_->is_visible_in_tree();
    out << ",\"panel_rect\":";
    rect_json(panel_shown ? panel_->get_global_rect() : Rect2());
    // The shown header labels (global pixels, the label's scale applied): #8031 keeps each off every icon frame.
    // The text the labels draw: the glyph pixels at the header scale, the least height a label strip can have.
    out << ",\"header_line_pixels\":" << header_line_;
    out << ",\"header_text_pixels\":";
    out << (headers_.empty() || !layout_ ? 0.0F : static_cast<float>(headers_.front()->glyph_pixels()) * static_cast<float>(layout_->header_scale));
    out << ",\"header_rects\":[";
    std::size_t headers_written{};
    for (std::size_t index = 1; index < headers_.size(); ++index) {
        const auto* label = headers_[index];
        if (!panel_shown || !label->is_visible_in_tree()) continue;
        out << (headers_written++ ? "," : "");
        rect_json(Rect2(label->get_global_position(), label->get_size() * label->get_scale()));
    }
    out << ']';
    // The text of each shown header, in the order of the rects (a tray never shows two identical headers, GBT-05).
    out << ",\"header_texts\":[";
    headers_written = 0;
    for (std::size_t index = 1; index < headers_.size(); ++index) {
        const auto* label = headers_[index];
        if (!panel_shown || !label->is_visible_in_tree()) continue;
        out << (headers_written++ ? "," : "") << '"' << escaped(std::string(label->get_text().utf8().get_data())) << '"';
    }
    out << ']';
    out << ",\"bar_rect\":";
    rect_json(bar_ ? Rect2(state_ && state_->command_bar ? state_->command_bar->get_global_position() + bar_->position : bar_->position, bar_->size) : Rect2());
    const char* width_mode = !layout_ ? "" : layout_->width_mode == tray::WidthMode::fixed ? "fixed"
        : layout_->width_mode == tray::WidthMode::bar ? "bar" : "auto";
    out << ",\"width_mode\":\"" << width_mode << "\",\"entry_scale\":"
        << (!layout_ ? 0.0 : compact_ ? layout_->compact_entry_scale : layout_->entry_scale)
        << ",\"backdrop\":{\"source\":\"" << (panel_shown ? backdrop_source_ : std::string{}) << "\",\"back_layers\":" << back_layers_
        << ",\"frame_layers\":" << frame_layers_ << ",\"back_sample\":[" << back_sample_.x << ',' << back_sample_.y << "]}"
        << ",\"planet_info_left\":" << (state_ ? state_->planet_info_card_left().value_or(-1.0F) : -1.0F)
        << ",\"close\":{\"art\":\"" << (close_art_.empty() ? std::string("bar_button") : close_art_) << "\",\"visible\":" << (close_ && close_->is_visible_in_tree() ? "true" : "false")
        << ",\"presses\":" << close_presses_ << ",\"tooltip\":\"";
    const std::string tip = close_ ? std::string(close_->get_tooltip_text().utf8().get_data()) : std::string{};
    for (const char c : tip) {
        if (c == '"' || c == '\\') out << '\\';
        if (c == '\n') { out << "\\n"; continue; }
        out << c;
    }
    out << "\",\"rect\":";
    rect_json(close_ && close_->is_visible_in_tree() ? close_->get_global_rect() : Rect2());
    out << '}';
    // The shown entries with their choice, for the GPU cases that click one and compare with the bar.
    out << ",\"shown_entries\":[";
    std::size_t written{};
    for (const auto& entry : entries_) {
        if (!entry.shown || !entry.button->is_visible_in_tree()) continue;
        std::string type;
        for (const char c : entry.choice.type) { if (c == '"' || c == '\\') type += '\\'; type += c; }
        out << (written++ ? "," : "") << "{\"type\":\"" << type << "\",\"type_id\":" << entry.choice.type_id
            << ",\"price\":" << (entry.choice.price ? std::to_string(entry.choice.price->trunc_to_integer()) : std::string("null"))
            << ",\"enabled\":" << (entry.choice.enabled ? "true" : "false") << ",\"rect\":";
        rect_json(entry.button->get_global_rect());
        out << '}';
    }
    out << "]}";
    return out.str();
}

} // namespace eawr::presentation::godot_backend
