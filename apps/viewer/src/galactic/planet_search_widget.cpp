#include "planet_search_widget.hpp"
#include "galactic_mode_internal.hpp"
#include "ui/input_routing.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/core/json_text.hpp"

#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/item_list.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/text_server.hpp>
#include <godot_cpp/classes/v_scroll_bar.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace {
std::string utf8(const String& value) { return value.utf8().get_data(); }
// PS-04: the owner-emblem column's clearance from the scroll bar, from the text height (no pixel size).
int32_t emblem_gap(const int32_t height) { return (height + 3) / 4; }
std::wstring wide(const std::string& value) {
    const auto text = String::utf8(value.c_str()).to_lower();
    std::wstring result;
    for (int64_t i = 0; i < text.length(); ++i) {
        const auto point = static_cast<std::uint32_t>(text[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (point > 0xffff) {
                result.push_back(static_cast<wchar_t>(0xd800 + ((point - 0x10000) >> 10)));
                result.push_back(static_cast<wchar_t>(0xdc00 + ((point - 0x10000) & 0x3ff)));
                continue;
            }
        }
        result.push_back(static_cast<wchar_t>(point));
    }
    return result;
}
const std::locale& search_locale() {
    static const std::locale locale = [] {
        try { return std::locale(""); } catch (const std::runtime_error&) { return std::locale::classic(); }
    }();
    return locale;
}
}

bool PlanetSearchWidget::visible() const { return root_ && root_->is_visible(); }

void PlanetSearchWidget::close() {
    if (root_) root_->hide();
    if (edit_) edit_->release_focus();
    if (list_) list_->release_focus();
}

void PlanetSearchWidget::open(GalacticMode::State& state) {
    state_ = &state;
    if (!state.planet_search_layout || !state.hud || !state.battle_dialog_builder) return;
    if (!root_) {
        root_ = memnew(EawrUiModalLayer);
        root_->set_name("PlanetSearch");
        root_->set_z_index(20);
        state.hud->add_child(root_);
        root_->hide();
    }
    root_->show();
    if (!dialog_ || size_ != state.hud->get_size()) rebuild();
    if (!edit_ || !list_) { close(); return; }
    edit_->set_text("");
    dirty_ = true;
    service(state);
    edit_->grab_focus();
    ++opens_;
}

void PlanetSearchWidget::rebuild() {
    if (dialog_ && dialog_->frame) { dialog_->frame->hide(); dialog_->frame->queue_free(); }
    dialog_.reset(); edit_ = nullptr; list_ = nullptr; emblems_.clear();
    size_ = state_->hud->get_size();
    auto built = state_->battle_dialog_builder("IDD_EAWR_PLANET_SEARCH", size_);
    if (!built.frame) return;
    dialog_ = std::move(built);
    root_->add_child(dialog_->frame);
    for (auto& gadget : dialog_->gadgets) {
        if (!gadget.control) continue;
        if (gadget.id == "IDC_PLANET_SEARCH_TEXT") edit_ = Object::cast_to<LineEdit>(gadget.control);
        if (gadget.id == "IDC_PLANET_SEARCH_RESULTS") list_ = Object::cast_to<ItemList>(gadget.control);
        if (gadget.id == "IDC_PLANET_SEARCH_CLOSE") {
            if (const auto* button = Object::cast_to<EawrUiButton>(gadget.control)) close_text_ = utf8(button->get_text());
            gadget.control->connect("pressed", callable_mp_static(+[](int64_t self) {
                reinterpret_cast<PlanetSearchWidget*>(self)->close();
            }).bind(reinterpret_cast<int64_t>(this)));
        }
    }
    if (!edit_ || !list_) return;
    edit_->connect("text_changed", callable_mp_static(+[](String, int64_t self) {
        reinterpret_cast<PlanetSearchWidget*>(self)->dirty_ = true;
    }).bind(reinterpret_cast<int64_t>(this)));
    for (Control* control : {static_cast<Control*>(edit_), static_cast<Control*>(list_)})
        control->connect("gui_input", callable_mp_static(+[](Ref<InputEvent> event, int64_t self) {
            reinterpret_cast<PlanetSearchWidget*>(self)->key(event);
        }).bind(reinterpret_cast<int64_t>(this)));
    list_->connect("item_clicked", callable_mp_static(+[](int64_t row, Vector2, int64_t button, int64_t self) {
        if (button == MOUSE_BUTTON_LEFT) reinterpret_cast<PlanetSearchWidget*>(self)->choose(row);
    }).bind(reinterpret_cast<int64_t>(this)));
    list_->set_max_columns(1);
    list_->set_text_overrun_behavior(TextServer::OVERRUN_TRIM_ELLIPSIS);
    list_->set_clip_contents(true);
    dirty_ = true;
}

void PlanetSearchWidget::key(const Ref<InputEvent>& event) {
    const auto* pressed = Object::cast_to<InputEventKey>(event.ptr());
    if (!visible() || !pressed || !pressed->is_pressed()) return;
    const auto code = pressed->get_keycode();
    if (code == KEY_F3 && !pressed->is_echo()) {
        edit_->accept_event();
        state_->set_perf_overlay(state_->perf == nullptr || !state_->perf->shown());
        return;
    }
    if (code != KEY_ESCAPE && code != KEY_ENTER && code != KEY_KP_ENTER && code != KEY_UP && code != KEY_DOWN) return;
    // Mark handled before close/selection can change focus, so Escape never
    // also opens the options menu and Enter never activates another control.
    if (edit_->has_focus()) edit_->accept_event(); else list_->accept_event();
    if (code == KEY_ESCAPE) { close(); return; }
    if (dirty_) refresh();
    const auto selected = list_->get_selected_items();
    int64_t row = selected.is_empty() ? -1 : selected[0];
    if (code == KEY_ENTER || code == KEY_KP_ENTER) { if (!pressed->is_echo()) choose(row); return; }
    if (matches_.empty()) return;
    row = std::clamp<int64_t>(row + (code == KEY_DOWN ? 1 : -1), 0, static_cast<int64_t>(matches_.size()) - 1);
    list_->select(static_cast<int32_t>(row));
    list_->ensure_current_is_visible();
}

void PlanetSearchWidget::choose(const int64_t row) {
    if (!visible() || row < 0 || static_cast<std::size_t>(row) >= matches_.size()) return;
    const auto id = matches_[static_cast<std::size_t>(row)];
    // Revalidate against the current observer, including a discovery/seat
    // change between drawing this list and the native input event.
    const auto planets = state_->input_planets();
    const auto found = std::find_if(planets.begin(), planets.end(), [id](const auto& planet) { return planet.id == id; });
    if (found == planets.end() || !found->discovered || state_->galaxy_modal_open()
        || state_->tactical || state_->loading || state_->tour || state_->map_input.gates.selection_locked
        || state_->map_input.gates.tutorial_locked || !state_->map_input.gates.world_allowed()) return;
    auto effect = state_->map_input.select(planets, id, state_->seat_flight().state());
    if (!effect.accepted) return;
    effect.position = true; // exactly the Page Up/Down camera-to-planet path
    // PS-06: the box stays open with its filter, selection and scroll; only Close or Escape closes it.
    state_->apply_selection(effect);
    state_->apply_camera();
    if (visible() && !edit_->has_focus() && !list_->has_focus()) edit_->grab_focus();
    last_pick_ = id; ++picks_;
}

void PlanetSearchWidget::refresh() {
    if (!edit_ || !list_) return;
    matches_ = index_.filter(utf8(edit_->get_text().to_lower()));
    list_->clear();
    // PS-04: Row_Scale enlarges the row text; both icons keep the measured text height. The kit list draws
    // with its List_Box role (and re-applies it on theme changes), so the role is overridden on the list
    // itself, scaled from the list's own size before the first override (kept per built list).
    const auto style = kit_text_style(*list_, "List_Box");
    if (row_list_ != list_->get_instance_id()) { row_list_ = list_->get_instance_id(); row_base_size_ = style.size; }
    const int size = std::max(1, static_cast<int>(std::lround(static_cast<float>(row_base_size_) * state_->planet_search_layout->row_scale)));
    if (style.size != size) list_->add_theme_font_size_override("List_Box", size);
    const int height = std::max(1, style.font.is_valid() ? static_cast<int>(std::ceil(style.font->get_height(size))) : size);
    list_->set_fixed_icon_size({height, height});
    // Partly scrolled rows and the emblem controls stay inside the list rectangle.
    list_->set_clip_contents(true);
    if (state_->planet_search_layout->picture && placeholder_height_ != height) {
        auto image = Image::create_empty(height, height, false, Image::FORMAT_RGBA8);
        image->fill(Color(0, 0, 0, 0));
        const auto color = list_->get_theme_color("font_color");
        const float radius = static_cast<float>(height) * 0.4F;
        const float center = static_cast<float>(height - 1) * 0.5F;
        for (int y = 0; y < height; ++y) for (int x = 0; x < height; ++x) {
            const float dx = static_cast<float>(x) - center, dy = static_cast<float>(y) - center;
            if (dx * dx + dy * dy <= radius * radius)
                image->set_pixel(x, y, Color(color.r, color.g, color.b, color.a * 0.35F));
        }
        placeholder_ = ImageTexture::create_from_image(image);
        placeholder_height_ = height;
    }
    const auto room = list_->get_size().x - list_->get_v_scroll_bar()->get_combined_minimum_size().x;
    const bool emblem = state_->planet_search_layout->emblem;
    // The emblem keeps a quarter text height clear of the scroll-bar column.
    list_->set_fixed_column_width(static_cast<int32_t>(std::max(1.0F, room - (emblem ? height + emblem_gap(height) : 0))));
    const auto query = utf8(edit_->get_text().to_lower());
    for (const auto& name : index_.entries()) {
        if (name.folded.find(query) == std::string::npos) continue;
        const auto picture = pictures_.find(name.id);
        const auto texture = state_->planet_search_layout->picture && picture != pictures_.end()
            ? (picture->second.empty() ? placeholder_ : state_->command_texture(picture->second))
            : (state_->planet_search_layout->picture ? placeholder_ : Ref<Texture2D>{});
        // The row already shows its whole name: no hover tooltip repeating it beside the cursor.
        list_->set_item_tooltip_enabled(list_->add_item(String::utf8(name.name.c_str()), texture), false);
    }
    if (!matches_.empty()) list_->select(0);
    dirty_ = false;
}

void PlanetSearchWidget::service(GalacticMode::State& state) {
    if (!visible()) return; // no list, discovery scans or texture work while closed
    if (!state.galaxy || state.tactical || state.loading || state.completed || state.tour || state.galaxy_modal_open()
        || !state.map_input.gates.world_allowed() || state.map_input.gates.tutorial_locked) { close(); return; }
    state_ = &state;
    if (size_ != state.hud->get_size()) {
        const auto text = edit_ ? edit_->get_text() : String{};
        rebuild();
        if (edit_) { edit_->set_text(text); edit_->grab_focus(); }
    }
    if (!dialog_ || !list_) return;
    const auto scale = static_cast<float>(dialog_->layout.scale_y);
    const auto& layout = *state.planet_search_layout;
    const auto size = dialog_->frame->get_size();
    dialog_->frame->set_position({std::max(0.0F, size_.x - size.x - layout.inset_x * scale),
        std::clamp(layout.inset_y * scale, 0.0F, std::max(0.0F, size_.y - size.y))});
    std::vector<galactic_view::PlanetSearchEntry> names;
    for (const auto& planet : state.input_planets()) {
        if (!planet.discovered) continue;
        const auto fact = state.hud_facts.find(planet.id);
        if (fact == state.hud_facts.end()) continue;
        const auto& name = fact->second.display_name;
        names.push_back({planet.id, name, utf8(String::utf8(name.c_str()).to_lower()), true});
        if (!pictures_.contains(planet.id) && state.audio_catalog) {
            // A mod may supply a 2D GUI/encyclopedia icon. Stock planet data
            // usually supplies only a model; those rows use the provider seam.
            std::string icon;
            const auto source = std::find_if(state.galaxy->planets.begin(), state.galaxy->planets.end(),
                [&](const auto& row) { return row.id == planet.id; });
            if (source != state.galaxy->planets.end()) {
                const auto type = state.audio_catalog->resolve(source->name, data::Category::game_object);
                if (type) if (const auto* value = type.value().value("Icon_Name")) icon = core::trim(value->value.raw_text, core::xml_whitespace);
            }
            pictures_.emplace(planet.id, std::move(icon));
        }
    }
    if (index_.update(std::move(names), [](const auto& a, const auto& b) {
        const auto left = wide(a), right = wide(b);
        const auto& collation = std::use_facet<std::collate<wchar_t>>(search_locale());
        return collation.compare(left.data(), left.data() + left.size(), right.data(), right.data() + right.size()) < 0;
    })) dirty_ = true;
    if (dirty_) refresh();
    const auto height = list_->get_fixed_icon_size().y;
    std::size_t used{};
    // The native list virtualizes its own text/icons. Only visible owner
    // emblem controls exist; scrolling a large galaxy never creates one per row.
    // Item rectangles are in unscrolled content space: the onscreen rows (pictures and emblem places) are
    // the ones inside the list after the scroll offset, not the list's first rows.
    const auto scroll = static_cast<float>(list_->get_v_scroll_bar()->get_value());
    for (int32_t row = 0; row < list_->get_item_count(); ++row) {
        auto rect = list_->get_item_rect(row, true);
        rect.position.y -= scroll;
        if (rect.get_end().y <= 0 || rect.position.y >= list_->get_size().y) continue;
        if (layout.picture) {
            const auto id = matches_[static_cast<std::size_t>(row)];
            const auto picture = pictures_.find(id);
            Ref<Texture2D> texture;
            if (picture != pictures_.end() && !picture->second.empty()) texture = state.command_texture(picture->second);
            else if (state.planet_search_thumbnails) texture = state.planet_search_thumbnails->thumbnail(id, height);
            if (texture.is_null()) texture = placeholder_;
            // Retain the provider's texture itself, so live textures keep updating.
            list_->set_item_icon(row, texture);
        }
        if (!layout.emblem) continue;
        if (used == emblems_.size()) {
            auto* icon = memnew(TextureRect);
            icon->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
            icon->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
            icon->set_stretch_mode(TextureRect::STRETCH_KEEP_ASPECT_CENTERED);
            list_->add_child(icon); emblems_.push_back(icon);
        }
        auto* icon = emblems_[used++];
        const auto facts = state.hud_facts.find(matches_[static_cast<std::size_t>(row)]);
        const auto* owner = state.content && facts != state.hud_facts.end() && facts->second.apparent_owner
            ? galactic_view::affiliation_for_owner(*state.content, facts->second.apparent_owner) : nullptr;
        auto texture = owner ? state.command_texture_optional(owner->icon) : Ref<Texture2D>{};
        // A faction's world Icon_Name need not be a UI texture. Reuse the
        // command bar's authored emblem table and its mod-defined side names.
        if (owner && texture.is_null()) if (const auto* style = state.command_catalog.find("Text_Allegiance_A")) {
            const auto textures = style->list(data::ui::Field::icon_alternate_texture_name);
            const auto index = presentation::ui::allegiance_emblem_index(owner->faction, state.content->side_names);
            if (index < textures.size()) texture = state.command_texture(textures[index]);
        }
        icon->set_texture(texture);
        const float right = list_->get_size().x - list_->get_v_scroll_bar()->get_combined_minimum_size().x;
        icon->set_position({right - static_cast<float>(height + emblem_gap(height)), rect.position.y + (rect.size.y - height) * 0.5F});
        icon->set_size({static_cast<float>(height), static_cast<float>(height)});
        icon->show();
    }
    while (used < emblems_.size()) emblems_[used++]->hide();
}

std::string PlanetSearchWidget::report_json() const {
    std::ostringstream out;
    out << "{\"visible\":" << (visible() ? "true" : "false") << ",\"opens\":" << opens_
        << ",\"picks\":" << picks_ << ",\"last_pick\":" << last_pick_
        << ",\"close_text\":" << core::json_quote(close_text_) << ",\"matches\":[";
    for (std::size_t row = 0; row < matches_.size(); ++row) { if (row) out << ','; out << matches_[row]; }
    out << "]}";
    return out.str();
}
} // namespace eawr::presentation::godot_backend
