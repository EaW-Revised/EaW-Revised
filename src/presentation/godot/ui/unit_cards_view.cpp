#include "unit_cards_view.hpp"

#include "eawr/presentation/ui/fonts.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/transform2d.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

// L-7: the debug build uses this atlas entry for a type without Icon_Name.
constexpr const char* fallback_portrait = "i_button_temporary.tga";
// The encyclopedia popup's header line in shell units, measured on a 1280x720 retail still.
constexpr data::ui::ReferenceRect encyclopedia_header{62.0F, 471.5F, 265.0F, 19.0F};

// CARD-19: the authored header width is a character budget, before pixel layout.
// A header without a price receives eight additional character positions.
[[nodiscard]] String wrapped_header(String title, const int64_t budget) {
    if (budget < 1) return title;
    int64_t line_start = 0;
    int64_t last_space = -1;
    for (int64_t index = 0; index < title.length(); ++index) {
        if (title[index] == U'\n') {
            line_start = index + 1;
            last_space = -1;
        } else {
            if (title[index] == U' ') last_space = index;
            if (index - line_start >= budget && last_space >= line_start) {
                title[last_space] = U'\n';
                line_start = last_space + 1;
                last_space = -1;
            }
        }
    }
    return title;
}

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] Rect2 aligned_bar(Rect2 rect) {
    // WSU-60: card bars keep Pixel_Align's default. Godot uses pixel edges,
    // so the D3D9 half-texel adjustment becomes an integral origin here.
    // Round the extent independently of the row's position to keep equal heights.
    rect.position.x = std::floor(rect.position.x);
    rect.position.y = std::floor(rect.position.y);
    rect.size.x = std::round(rect.size.x);
    rect.size.y = std::round(rect.size.y);
    return rect;
}

[[nodiscard]] std::string json_space_controls_with_newline(const std::string_view value) {
    std::string out{"\""};
    for (const char character : value) {
        if (character == '\n') { out += "\\n"; continue; }
        if (character == '"' || character == '\\') out += '\\';
        out += static_cast<unsigned char>(character) < 0x20U ? ' ' : character;
    }
    return out + "\"";
}

[[nodiscard]] std::string rect_json(const Rect2& rect) {
    std::ostringstream output;
    output << "[" << rect.position.x << ", " << rect.position.y << ", " << rect.size.x << ", " << rect.size.y << "]";
    return output.str();
}

// A quad of texel size times `scale` centred on `centre`, in shell units.
[[nodiscard]] data::ui::ReferenceRect centred(const assets::Vec2f centre, const Vector2 texels, const float scale) {
    const float width = texels.x * scale;
    const float height = texels.y * scale;
    return {centre.x - width / 2.0F, centre.y - height / 2.0F, width, height};
}

} // namespace

EawrUnitCards::EawrUnitCards() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
    set_anchors_and_offsets_preset(PRESET_FULL_RECT);
}

EncyclopediaPopupRenderer::~EncyclopediaPopupRenderer() {
    if (auto* rendering = RenderingServer::get_singleton(); rendering && popup_glow_item_.is_valid())
        rendering->free_rid(popup_glow_item_);
}
void EncyclopediaPopupRenderer::initialize(Control& owner) {
    if (!popup_text_owner_) {
        popup_text_owner_ = memnew(Control);
        popup_text_owner_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        owner.add_child(popup_text_owner_);
    }
    if (!popup_glow_item_.is_valid()) {
        auto* rendering = RenderingServer::get_singleton();
        popup_glow_item_ = rendering->canvas_item_create();
        rendering->canvas_item_set_parent(popup_glow_item_, owner.get_canvas_item());
        rendering->canvas_item_set_draw_behind_parent(popup_glow_item_, true);
        Ref<CanvasItemMaterial> material;
        material.instantiate();
        material->set_blend_mode(CanvasItemMaterial::BLEND_MODE_ADD);
        popup_glow_material_ = material;
        rendering->canvas_item_set_material(popup_glow_item_, material->get_rid());
    }
}
void EncyclopediaPopupRenderer::clear() {
    popup_rect_ = Rect2();
    for (auto& line : popup_text_) line.draw(*popup_text_owner_, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1, 1);
    for (auto& line : popup_cell_text_) line->draw(*popup_text_owner_, KitTextStyle{}, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1, 1);
}
EawrUnitCards::~EawrUnitCards() = default;
void EawrUnitCards::setup(Setup setup) {
    setup_ = std::move(setup);
    popup_.initialize(*this);
    timing_ = setup_.timing.value_or(model::EncyclopediaTiming{});
    serviced_at_ = now();
    cells_.clear();
    for (std::size_t index = 0; index < setup_.slots.size(); ++index) cells_.push_back(std::make_unique<PortraitCellRenderer>());
    set_process(true);
    queue_redraw();
}

void EawrUnitCards::show(std::vector<Card> cards, std::vector<model::CardBorder> borders) {
    if (!cards.empty() && cards.front().price) ++build_card_batches_;
    const auto same_card = [](const Card& a, const Card& b) {
        return a.slot == b.slot && a.type == b.type && a.count == b.count && a.stacked == b.stacked
            && a.health_level == b.health_level && a.shield == b.shield && a.price == b.price && a.room == b.room
            && a.pad == b.pad && a.cooldown_progress == b.cooldown_progress
            && a.build_frames == b.build_frames
            && a.disabled == b.disabled && a.disabled_reason == b.disabled_reason;
    };
    const bool same = cards.size() == cards_.size() && std::equal(cards.begin(), cards.end(), cards_.begin(), same_card)
        && borders.size() == borders_.size()
        && std::equal(borders.begin(), borders.end(), borders_.begin(), [](const auto& a, const auto& b) {
               return a.column == b.column && a.piece == b.piece;
           });
    if (same) return;
    const bool slots_changed = cards.size() != cards_.size()
        || !std::equal(cards.begin(), cards.end(), cards_.begin(),
                       [](const Card& a, const Card& b) { return a.slot == b.slot && a.type == b.type; });
    cards_ = std::move(cards);
    borders_ = std::move(borders);
    if (slots_changed) {
        // A card that changed under the pointer restarts its hover.
        hovered_since_ = now();
        if (!shown_external_) {
            timing_.candidate = timing_.shown = 0;
            timing_.hovered_seconds = timing_.alpha = 0;
            tooltip_shown_ = false;
            shown_card_.reset();
        }
    }
    update_text();
    queue_redraw();
}

Rect2 EawrUnitCards::screen(const data::ui::ReferenceRect& rect) const {
    return rect2(model::shell_to_screen(rect, setup_.placement()));
}

bool EawrUnitCards::update_build_states(const std::span<const model::BuildButton> buttons) {
    if (buttons.size() != cards_.size()) return false;
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        if (cards_[index].slot != buttons[index].slot || !cards_[index].price) return false;
    }
    ++build_state_updates_;
    bool text_changed{};
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        auto& card = cards_[index];
        const auto& button = buttons[index];
        // A listed upgrade's zero price remains resolved from its XML by show().
        if (button.price != 0 && card.price != button.price) {
            card.price = button.price;
            text_changed = true;
        }
        text_changed = text_changed || card.room != button.room || card.disabled != !button.enabled;
        card.room = button.room;
        card.disabled = !button.enabled;
        if (card.disabled_reason != button.disabled_reason) {
            card.disabled_reason = button.disabled_reason;
            text_changed = true;
        }
        if (card.build_frames != button.build_frames) {
            card.build_frames = button.build_frames;
            text_changed = true;
        }
        card.pad = button.pad;
        card.cooldown_progress = button.cooldown_progress;
    }
    if (text_changed) update_text();
    queue_redraw();
    return true;
}

std::optional<Rect2> EawrUnitCards::slot_rect(const std::size_t slot) const {
    if (slot >= setup_.slots.size() || !setup_.placement) return std::nullopt;
    return screen(setup_.slots[slot].card.rect);
}

std::optional<std::size_t> EawrUnitCards::slot_at(const Vector2& point) const {
    for (std::size_t slot = 0; slot < setup_.slots.size(); ++slot) {
        const auto rect = slot_rect(slot);
        if (rect && rect->has_point(point)) return slot;
    }
    return std::nullopt;
}

bool EawrUnitCards::_has_point(const Vector2& point) const { return slot_at(point).has_value(); }

void EawrUnitCards::set_external_hover(const std::uint64_t identity, std::optional<model::UnitCardLooks> content) {
    if (content && shown_external_ && external_identity_ == identity) {
        shown_external_content_ = content;
        update_text();
        queue_redraw();
    }
    if (!content && shown_external_) {
        timing_.shown = 0;
        timing_.alpha = 0;
        tooltip_shown_ = false;
        shown_card_.reset();
        shown_external_ = false;
        update_text();
        queue_redraw();
    }
    external_identity_ = content ? identity : 0;
    external_content_ = std::move(content);
}

double EawrUnitCards::now() const {
    if (clock_) return clock_();
    return static_cast<double>(Time::get_singleton()->get_ticks_usec()) / 1.0e6;
}

Ref<Texture2D> EawrUnitCards::portrait(const std::string& type, std::string* name) const {
    std::string icon = setup_.icon ? setup_.icon(type) : std::string();
    if (icon.empty()) icon = fallback_portrait;
    if (name != nullptr) *name = icon;
    return setup_.texture ? setup_.texture(icon) : Ref<Texture2D>();
}

void EawrUnitCards::act(const Vector2& at, const bool shift) {
    const auto slot = slot_at(at);
    if (!slot) return;
    ++clicks_;
    if (click_) click_(*slot, shift);
}

void EawrUnitCards::_gui_input(const Ref<InputEvent>& event) {
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        const auto slot = slot_at(motion->get_position());
        if (slot != hovered_) {
            hovered_ = slot;
            hovered_since_ = now();
            if (tooltip_shown_) {
                tooltip_shown_ = false;
                update_text();
                queue_redraw();
            }
        }
        return;
    }
    const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr());
    if (button == nullptr || button->get_button_index() != MOUSE_BUTTON_LEFT) return;
    accept_event();
    // The debug build's command bar mouse handling (CARD-5): a component's left action runs on the release over it;
    // a double click runs it at once and the release after it does nothing.
    if (button->is_pressed()) {
        if (button->is_double_click()) {
            ++double_clicks_;
            spend_release_ = true;
            act(button->get_position(), button->is_shift_pressed());
        }
        return;
    }
    if (spend_release_) {
        spend_release_ = false;
        return;
    }
    act(button->get_position(), button->is_shift_pressed());
}

void EawrUnitCards::_notification(const int what) {
    if (what == NOTIFICATION_RESIZED) {
        update_text();
        queue_redraw();
    } else if (what == NOTIFICATION_MOUSE_EXIT) {
        hovered_.reset();
        // CARD-18: the shared timer owns minimum display and fade-out.
    } else if (what == NOTIFICATION_PROCESS) {
        if (get_size() != laid_out_) {
            laid_out_ = get_size();
            update_text();
            queue_redraw();
        }
        if (!setup_.timing) return;
        const auto card = std::find_if(cards_.begin(), cards_.end(), [&](const Card& value) { return hovered_ && value.slot == *hovered_; });
        const bool external = card == cards_.end() && external_content_.has_value();
        const std::uint64_t candidate = card != cards_.end() ? card->slot + 1U
            : external ? (external_identity_ | (std::uint64_t{1} << 63U)) : 0U;
        const double at = now();
        const auto old = timing_.shown;
        const auto alpha = timing_.alpha;
        timing_.service(candidate, std::max(0.0, at - serviced_at_), is_visible_in_tree());
        serviced_at_ = at;
        if (old != timing_.shown && timing_.shown == candidate && candidate) {
            shown_external_ = external;
            if (external) {
                shown_card_ = Card{};
                shown_card_->type = external_content_->name;
                shown_external_content_ = external_content_;
            } else shown_card_ = *card;
        }
        tooltip_shown_ = timing_.shown != 0;
        if (!tooltip_shown_) shown_card_.reset();
        if (old != timing_.shown) update_text();
        if (old != timing_.shown || alpha != timing_.alpha) {
            popup_.popup_text_owner_->set_modulate(Color(1, 1, 1, static_cast<float>(timing_.alpha)));
            queue_redraw();
        }
    }
}

void EawrUnitCards::update_text() {
    if (!setup_.placement || !setup_.space) return;
    if (shown_card_ && !shown_external_) {
        const auto current = std::find_if(cards_.begin(), cards_.end(), [&](const Card& card) {
            return card.slot == shown_card_->slot && card.type == shown_card_->type;
        });
        if (current != cards_.end()) shown_card_ = *current;
    }
    const model::ShellPlacement shell = setup_.placement();
    const model::FontPixels pixels = model::font_pixels({setup_.point_size, false, 1.0}, model::font_screen_height(setup_.space()));
    KitTextStyle style;
    style.font = setup_.font;
    style.size = pixels.glyph_height;
    style.top = style.bottom = Color(1, 1, 1, 1);
    if (setup_.cell) {
        const model::TextCell cell = setup_.cell(pixels.glyph_height);
        style.cell_ascent = cell.ascent;
        style.cell_descent = cell.descent;
    }
    for (std::size_t slot = 0; slot < setup_.slots.size(); ++slot) {
        const model::HudCardSlot& look = setup_.slots[slot];
        const auto card = std::find_if(cards_.begin(), cards_.end(), [slot](const Card& shown) { return shown.slot == slot; });
        String text;
        if (card != cards_.end() && card->stacked) text = String("x") + String::num_int64(card->count);
        style.outline = look.outline;
        style.top = style.bottom = Color(look.colour.r / 255.0F, look.colour.g / 255.0F, look.colour.b / 255.0F, 1.0F);
        if (card != cards_.end() && card->price) {
            // #530 PU-62: the build price (unverified: drawn where a card's count sits).
            text = String::num_int64(*card->price);
            const float grey = card->room ? 1.0F : 128.0F / 255.0F;
            style.top = style.bottom = Color(grey, grey, grey, 200.0F / 255.0F);
        }
        // Text 2 sits centred on the bone plus Text_Offset2; a two-line-high box keeps it centred.
        const assets::Vec2f at{look.card.origin.x + look.count_offset.x, look.card.origin.y + look.count_offset.y};
        const Rect2 box = screen({at.x - 25.0F, at.y - 6.0F, 50.0F, 12.0F});
        cells_[slot]->draw_text(*this, {style, text, box, HORIZONTAL_ALIGNMENT_CENTER,
                                       static_cast<float>(shell.scale)});
    }
    String name;
    tooltip_rect_ = Rect2();
    popup_.clear();
    popup_.popup_cell_rects_.clear();
    if (tooltip_shown_ && shown_card_) {
        const Card* card = &*shown_card_;
        if (setup_.name) {
            if (!card->pad && setup_.encyclopedia) {
                tooltip_.draw(*this, style, String(), Rect2(), HORIZONTAL_ALIGNMENT_LEFT, false, false, 1.0F, 1.0F);
                update_popup(*card);
                return;
            }
            name = setup_.name(card->type);
            if (card->build_frames) {
                name += " (" + String::num(static_cast<double>(*card->build_frames) / 30.0, 1) + " s)";
            }
            if (!card->disabled_reason.empty()) name += "\n" + String(card->disabled_reason.c_str());
            const model::FontPixels large = model::font_pixels({10, false, 1.0}, model::font_screen_height(setup_.space()));
            style.size = large.glyph_height;
            if (setup_.cell) {
                const model::TextCell cell = setup_.cell(large.glyph_height);
                style.cell_ascent = cell.ascent;
                style.cell_descent = cell.descent;
            }
            style.outline = true;
            style.top = style.bottom = Color(1, 1, 1, 1);
            // The retail still (card_hover, #425) opens the encyclopedia above the help droid, its
            // header line at about (62, 472)..(327, 491) shell units; the name takes that line.
            tooltip_rect_ = screen(encyclopedia_header);
            if (card->pad && setup_.tooltip_size.x > 0 && setup_.tooltip_size.y > 0 && card->slot < setup_.slots.size()) {
                if (setup_.description) {
                    const auto body = setup_.description(card->type);
                    if (!body.is_empty()) name += String("\n") + body;
                }
                style.size = model::font_pixels({setup_.tooltip_point_size, false, 1.0},
                    model::font_screen_height(setup_.space())).glyph_height;
                const auto& at = setup_.slots[card->slot].card.origin;
                const double line_width = style.font.is_valid() ? style.font->get_string_size(name, HORIZONTAL_ALIGNMENT_LEFT, -1, style.size).x : 0;
                const float lines = static_cast<float>(std::max(2.0, std::ceil(line_width / setup_.tooltip_size.x) + 1.0));
                const float height = setup_.tooltip_size.y * lines;
                tooltip_rect_ = screen({at.x - setup_.tooltip_size.x / 2,
                    setup_.slots[card->slot].card.rect.top() + setup_.tooltip_size.y,
                    setup_.tooltip_size.x, height});
            } else if (!card->disabled_reason.empty()) tooltip_rect_ = screen({62.0F, 391.0F, 650.0F, 100.0F});
        }
    }
    tooltip_.draw(*this, style, name, tooltip_rect_, HORIZONTAL_ALIGNMENT_CENTER, true, true, 1.0F, 1.0F);
}

void EawrUnitCards::update_popup(const Card& card) {
    ++popup_content_builds_;
    const auto content = shown_external_ && shown_external_content_ ? *shown_external_content_ : setup_.encyclopedia(card.type);
    // CARD-19: preserve the tactical anchor and its upward growth.
    constexpr float anchor_y = static_cast<float>((720.0 - 430.0) * model::reference_height / 720.0);
    const auto anchor = screen({64.0F, anchor_y, setup_.tooltip_size.x, 0.0F});
    popup_.update(setup_, content, {card.price, card.disabled_reason, card.price.has_value()},
        anchor.position, setup_.space(), static_cast<float>(setup_.placement().scale));
    tooltip_rect_ = popup_.popup_rect_;
}

void EncyclopediaPopupRenderer::update(const EncyclopediaPopupStyle& setup,
    const model::UnitCardLooks& content, const EncyclopediaPopupStatus& status,
    const Vector2& bottom_left, const model::ReferenceSpace& space, const float scale) {
    clear();
    popup_content_ = content;
    if (status.production) {
        // CARD-14/17: the retail build header supplies population and cost,
        // without the selection/world header's class line or static abilities.
        popup_content_.population = std::abs(popup_content_.population);
        popup_content_.unit_class.clear();
        popup_content_.ability_icons.clear();
    }
    const float width = setup.tooltip_size.x * scale;
    if (width <= 0.0F || setup.popup_body_font.is_null() || setup.popup_header_font.is_null()) return;
    const float padding = 4.0F * scale;
    const float inner = width - 2.0F * padding;
    KitTextStyle body;
    body.font = setup.popup_body_font;
    body.size = model::font_pixels({setup.popup_body_points, false, 1.0}, model::font_screen_height(space)).glyph_height;
    body.top = body.bottom = setup.popup_body_colour;
    // CARD-19: hover alpha replaces authored ink alpha, not multiplies it.
    body.top.a = body.bottom.a = 1.0F;
    KitTextStyle header = body;
    header.font = setup.popup_header_font;
    header.size = model::font_pixels({setup.popup_header_points, false, 1.0}, model::font_screen_height(space)).glyph_height;
    header.top = header.bottom = Color(1, 1, 1, 1);
    const float row = std::max(setup.tooltip_size.y * scale, static_cast<float>(body.font->get_height(body.size)));
    const float header_row = std::max(setup.popup_header_size.y * scale, static_cast<float>(header.font->get_height(header.size)));
    const auto art_size = [&](const std::string& name) {
        const auto texture = setup.texture ? setup.texture(name) : Ref<Texture2D>();
        return texture.is_valid() ? texture->get_size() : Vector2();
    };
    const float icon_scale = popup_content_.icon_scale > 0.0F ? popup_content_.icon_scale : setup.popup_icon_scale.x;
    const Vector2 portrait_size = art_size(popup_content_.icon) * icon_scale * scale;
    // WGUI-166: the fleet header icon sits on its stack texture, both at the icon scale.
    const Vector2 back_size = popup_content_.icon_back.empty() ? Vector2() : art_size(popup_content_.icon_back) * icon_scale * scale;
    KitTextStyle cost = header;
    cost.font = setup.popup_cost_font;
    cost.size = model::font_pixels({setup.popup_cost_points, false, 1.0}, model::font_screen_height(space)).glyph_height;
    cost.top = cost.bottom = setup.popup_cost_colour;
    const String price = status.price && *status.price > 0 ? String("$ ") + String::num_int64(*status.price) : String();
    const float cost_width = price.is_empty() || cost.font.is_null() ? 0.0F
        : static_cast<float>(cost.font->get_string_size(price, HORIZONTAL_ALIGNMENT_LEFT, -1, cost.size).x);
    const bool portrait_shown = !popup_content_.icon.empty();
    const float population_shift = popup_content_.population ? 0.0F : setup.popup_population_offset;
    const float name_offset = (portrait_shown ? setup.popup_name_offset - population_shift
        : popup_content_.population ? setup.popup_name_offset - (setup.popup_icon_x_offset - setup.popup_population_offset)
        : setup.popup_population_offset) * scale;
    const float cost_right = setup.popup_cost_offset * scale;
    const float name_width = std::max(1.0F, cost_right - name_offset - cost_width - padding);
    const int64_t title_budget = static_cast<int64_t>(setup.popup_header_size.x) + (price.is_empty() ? 8 : 0);
    const String title = wrapped_header(String::utf8(popup_content_.name.c_str()), title_budget);
    const float title_lines = std::max(1.0F, std::round(static_cast<float>(header.font->get_multiline_string_size(
        title, HORIZONTAL_ALIGNMENT_LEFT, name_width, header.size).y) / static_cast<float>(header.font->get_height(header.size))));
    const float title_height = title_lines * header_row;
    const float separator_height = art_size("e_line.tga").y * scale;
    const float header_height = std::max({portrait_size.y, back_size.y, title_height + separator_height});
    const auto string = [](const std::string& value) { return String::utf8(value.c_str()); };
    String description = string(popup_content_.description);
    if (!status.disabled_reason.empty()) description += String("\n") + String::utf8(status.disabled_reason.c_str());
    const float body_height = description.is_empty() ? 0.0F : body.font->get_multiline_string_size(
        description, HORIZONTAL_ALIGNMENT_LEFT, inner, body.size).y;
    const bool against = !popup_content_.good_against_icons.empty() || !popup_content_.vulnerable_to_icons.empty();
    const Vector2 panel_size = art_size("e_against_frame.tga") * scale;
    const float icon = panel_size.x / 3.0F;
    // WGUI-166: the fleet list applies encyclopedia_icon's Size X after
    // ordinary list-button setup, replacing its temporary icon scale. A
    // count-only cell takes the first resolved icon's size.
    Vector2 blank = art_size(fallback_portrait);
    for (const auto& cell : popup_content_.cells) {
        if (const auto size = cell.icon.empty() ? Vector2() : art_size(cell.icon); size.x > 0.0F) { blank = size; break; }
    }
    std::vector<Vector2> cell_sizes;
    float cells_height = 0.0F;
    float row_height = 0.0F;
    for (const auto& cell : popup_content_.cells) {
        Vector2 size = cell.icon.empty() ? Vector2() : art_size(cell.icon);
        if (size.x <= 0.0F) size = blank;
        size *= setup.popup_icon_scale.x;
        if (cell_sizes.size() % model::encyclopedia_cell_columns == 0) { cells_height += row_height; row_height = 0.0F; }
        cell_sizes.push_back(size * scale);
        row_height = std::max(row_height, std::ceil(size.y * scale));
    }
    if (!cell_sizes.empty()) cells_height += row_height + padding;
    const float height = padding * 3.0F + header_height + body_height + (against ? row + panel_size.y : 0.0F) + cells_height;
    popup_rect_ = Rect2(bottom_left - Vector2(0, height), Vector2(width, height));
    Vector2 at = popup_rect_.position + Vector2(padding, padding);
    const float population_width = setup.popup_population_offset * 2.0F * scale;
    popup_title_rect_ = Rect2(popup_rect_.position + Vector2(0, padding), Vector2(width, header_row));
    popup_population_rect_ = Rect2(popup_title_rect_.position, Vector2(population_width, header_row));
    popup_portrait_rect_ = Rect2(popup_title_rect_.position + Vector2(
        (setup.popup_icon_x_offset - population_shift) * scale,
        header_row / 2.0F - setup.popup_icon_y_offset * scale) - portrait_size / 2.0F, portrait_size);
    popup_back_rect_ = Rect2(popup_portrait_rect_.get_center() - back_size / 2.0F, back_size);
    popup_ability_rects_.clear();
    float ability_right = width - padding;
    for (const auto& name : popup_content_.ability_icons) {
        const Vector2 size = art_size(name) * setup.popup_icon_scale.y * scale;
        ability_right -= size.x;
        popup_ability_rects_.emplace_back(popup_title_rect_.position + Vector2(ability_right, title_height), size);
    }
    // CARD-14: production uses the provider's prepared cost; the inspected
    // retail header does not append a speculative build-time suffix.
    popup_name_rect_ = Rect2(popup_title_rect_.position + Vector2(name_offset, 0), Vector2(name_width, title_height));
    popup_text_[0].draw(*popup_text_owner_, header, title, popup_name_rect_,
        HORIZONTAL_ALIGNMENT_LEFT, false, true, scale, 1.0F);
    popup_text_[1].draw(*popup_text_owner_, body, string(popup_content_.unit_class),
        Rect2(popup_title_rect_.position + Vector2(name_offset, title_height + setup.popup_class_y_offset * scale),
            Vector2(std::max(0.0F, ability_right - name_offset), row)),
        HORIZONTAL_ALIGNMENT_LEFT, false, false, scale, 1.0F);
    KitTextStyle population_style = header;
    // CARD-17: sampled retail usage gold, distinct from capacity green.
    constexpr float population_usage_green = 215.0F / 255.0F;
    constexpr float population_usage_blue = 56.0F / 255.0F;
    population_style.top = population_style.bottom = popup_content_.population > 0
        ? Color(0, 1, 0, 1) : Color(1, population_usage_green, population_usage_blue, 1);
    popup_text_[5].draw(*popup_text_owner_, population_style, popup_content_.population
        ? String::num_int64(std::abs(popup_content_.population)) : String(), popup_population_rect_,
        HORIZONTAL_ALIGNMENT_CENTER, false, false, scale, 1.0F);
    popup_text_[6].draw(*popup_text_owner_, cost, price,
        Rect2(popup_title_rect_.position + Vector2(cost_right - cost_width, 0), Vector2(cost_width, row)),
        HORIZONTAL_ALIGNMENT_RIGHT, false, false, scale, 1.0F);
    at.y += header_height + padding;
    popup_text_[2].draw(*popup_text_owner_, body, description, Rect2(at, Vector2(inner, body_height)), HORIZONTAL_ALIGNMENT_LEFT,
        false, true, scale, 1.0F);
    at.y += body_height;
    for (std::size_t column = 0; column < 2; ++column) {
        const Vector2 origin(popup_rect_.position.x + static_cast<float>(column) * (panel_size.x + 4.0F * scale), at.y);
        body.top = body.bottom = column == 0 ? Color(0, 192.0F / 255.0F, 0, 1) : Color(192.0F / 255.0F, 0, 0, 1);
        popup_text_[3 + column].draw(*popup_text_owner_, body, against ? (column == 0 ? setup.good_against_text : setup.vulnerable_to_text) : String(),
            Rect2(origin, Vector2(panel_size.x, row)), HORIZONTAL_ALIGNMENT_LEFT, false, false, scale, 1.0F);
        popup_matchup_rects_[column] = Rect2(origin + Vector2(0, row), panel_size);
        for (std::size_t slot = 0; slot < 3; ++slot)
            popup_icon_rects_[column * 3 + slot] = Rect2(origin + Vector2(static_cast<float>(slot) * icon, row), Vector2(icon, icon));
    }
    // WGUI-166: six cells per row; "x<n>" from two units on encyclopedia_icon's Text_Offset.
    popup_cell_rects_.clear();
    while (popup_cell_text_.size() < cell_sizes.size()) popup_cell_text_.push_back(std::make_unique<KitText>());
    KitTextStyle caption;
    caption.font = setup.popup_cell_font.is_valid() ? setup.popup_cell_font : header.font;
    caption.size = model::font_pixels({setup.popup_cell_points, false, 1.0}, model::font_screen_height(space)).glyph_height;
    caption.top = caption.bottom = Color(1, 1, 1, 1);
    caption.outline = setup.popup_cell_outline;
    Vector2 cell_at(popup_rect_.position.x + padding, at.y + (against ? row + panel_size.y : 0.0F) + padding);
    row_height = 0.0F;
    for (std::size_t index = 0; index < popup_cell_text_.size(); ++index) {
        if (index >= cell_sizes.size()) {
            popup_cell_text_[index]->draw(*popup_text_owner_, caption, String(), Rect2(), HORIZONTAL_ALIGNMENT_CENTER, false, false, scale, 1.0F);
            continue;
        }
        if (index > 0 && index % model::encyclopedia_cell_columns == 0) {
            cell_at = Vector2(popup_rect_.position.x + padding, cell_at.y + row_height);
            row_height = 0.0F;
        }
        const Rect2 box(cell_at, cell_sizes[index]);
        popup_cell_rects_.push_back(box);
        const auto count = popup_content_.cells[index].count;
        const Vector2 centre = box.get_center() + Vector2(setup.popup_cell_offset.x, setup.popup_cell_offset.y) * scale;
        popup_cell_text_[index]->draw(*popup_text_owner_, caption, count >= 2 ? String("x") + String::num_int64(count) : String(),
            Rect2(centre - Vector2(25.0F, 6.0F) * scale, Vector2(50.0F, 12.0F) * scale), HORIZONTAL_ALIGNMENT_CENTER, true, false, scale, 1.0F);
        // WGUI-166: list cells advance by whole display pixels.
        cell_at.x += std::ceil(box.size.x);
        row_height = std::max(row_height, std::ceil(box.size.y));
    }
}

void EawrUnitCards::draw_bar(const model::HudBar& bar, const std::int32_t level, const double percent) {
    if (!setup_.texture) return;
    const assets::Vec2f centre{bar.origin.x + bar.offset.x, bar.origin.y + bar.offset.y};
    if (!bar.back.empty()) {
        const Ref<Texture2D> back = setup_.texture(bar.back[std::min<std::size_t>(bar.back.size() - 1, bar.max_level)]);
        if (back.is_valid()) draw_texture_rect(back, aligned_bar(screen(centred(centre, back->get_size(), bar.scale))), false);
    }
    if (bar.overlay.empty()) return;
    const std::size_t index = std::min<std::size_t>(bar.overlay.size() - 1, static_cast<std::size_t>(std::max(0, level)));
    const Ref<Texture2D> overlay = setup_.texture(bar.overlay[index]);
    if (overlay.is_null()) return;
    const Vector2 texels = overlay->get_size();
    Rect2 quad = aligned_bar(screen(centred(centre, texels, bar.scale)));
    if (bar.smooth) {
        // The debug build's bar component (CARD-4): the overlay narrows to the percent and keeps its left edge.
        quad.size.x *= static_cast<float>(std::clamp(percent, 0.0, 1.0));
    }
    if (quad.size.x <= 0.0F || quad.size.y <= 0.0F) return;
    draw_texture_rect(overlay, quad, false);
}

void EawrUnitCards::_draw() {
    if (popup_.popup_glow_item_.is_valid()) RenderingServer::get_singleton()->canvas_item_clear(popup_.popup_glow_item_);
    if (!setup_.placement || !setup_.texture) return;
    // Borders first (Base_Layer 2), then the cards (4) and their bars (5).
    for (const model::CardBorder& border : borders_) {
        if (border.column >= setup_.borders.size()) continue;
        const model::HudShellButton& look = setup_.borders[border.column];
        const auto piece = static_cast<std::size_t>(border.piece);
        if (piece >= look.alternates.size()) continue;
        const Ref<Texture2D> texture = setup_.texture(look.alternates[piece]);
        if (texture.is_null()) continue;
        const Vector2 size = texture->get_size();
        draw_texture_rect(texture, screen(model::button_quad(look, size.x, size.y)), false);
    }
    for (const Card& card : cards_) {
        if (card.slot >= setup_.slots.size()) continue;
        const model::HudCardSlot& look = setup_.slots[card.slot];
        const Ref<Texture2D> texture = portrait(card.type);
        if (texture.is_valid()) {
            const Vector2 size = texture->get_size();
            Color tint(look.colour.r / 255.0F, look.colour.g / 255.0F, look.colour.b / 255.0F, look.colour.a / 255.0F);
            if (card.pad) tint = Color(1, 1, 1, 1); // WBP-36
            if (card.price && card.disabled) tint = Color(128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F, 1.0F); // PU-61
            std::optional<PortraitCellRenderer::Progress> progress;
            if (card.pad && card.cooldown_progress < 1.0 && !look.marks.build.empty()) {
                const auto dial = setup_.texture(look.marks.build);
                if (dial.is_valid()) {
                    const auto bone = look.card.origin;
                    const auto dial_size = dial->get_size();
                    const float width = dial_size.x * look.card.scale;
                    const float height = dial_size.y * look.card.scale;
                    progress = PortraitCellRenderer::Progress{dial,
                        screen({bone.x + look.marks.dial.x - width / 2.0F,
                            bone.y + look.marks.dial.y - height / 2.0F, width, height}), card.cooldown_progress};
                }
            }
            PortraitCellRenderer::draw_portrait(*this, texture,
                screen(model::button_quad(look.card, size.x, size.y)), tint, progress);
        }
        if (card.stacked || card.price) continue; // a stacked card or a build button has no bars
        if (look.health) {
            const std::int32_t levels = std::max(1, look.health->max_level);
            draw_bar(*look.health, card.health_level, static_cast<double>(card.health_level) / levels);
        }
        if (look.shield && card.shield) {
            const std::int32_t levels = std::max(1, look.shield->max_level);
            const auto level = static_cast<std::int32_t>(std::ceil(static_cast<float>(levels) * static_cast<float>(*card.shield)));
            draw_bar(*look.shield, level, *card.shield);
        }
    }
    popup_.draw(*this, setup_, static_cast<float>(setup_.placement().scale), static_cast<float>(timing_.alpha));
    if (!popup_.popup_rect_.has_area() && tooltip_rect_.has_area()) draw_rect(tooltip_rect_, Color(0.0F, 0.0F, 0.0F, 0.75F));
}

void EncyclopediaPopupRenderer::draw(Control& owner, const EncyclopediaPopupStyle& setup,
    const float scale, const float alpha, const Vector2 offset) {
    if (popup_glow_item_.is_valid()) {
        RenderingServer::get_singleton()->canvas_item_clear(popup_glow_item_);
        RenderingServer::get_singleton()->canvas_item_set_transform(popup_glow_item_, Transform2D(0, -offset));
    }
    owner.draw_set_transform(-offset);
    if (popup_rect_.has_area()) {
        const auto draw_asset = [&](const std::string& name, const Rect2& box, Color tint = Color(1, 1, 1, 1)) {
            if (name.empty() || !setup.texture) return;
            const auto texture = setup.texture(name);
            tint.a *= alpha;
            if (texture.is_valid()) owner.draw_texture_rect(texture, box, false, tint);
        };
        // CARD-19: a one-unit edge, then an additive glow enlarged by 16 units.
        if (setup.texture && popup_glow_item_.is_valid()) {
            const auto glow = setup.texture(setup.popup_glow);
            if (glow.is_valid()) RenderingServer::get_singleton()->canvas_item_add_texture_rect(
                popup_glow_item_, popup_rect_.grow(8.0F * scale), glow->get_rid(), false,
                Color(128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F, 128.0F / 255.0F * alpha));
        }
        draw_asset(setup.popup_frame, popup_rect_.grow(scale));
        draw_asset(setup.popup_background, popup_rect_);
        // CARD-19: measured retail lower-left connector, in shell units; it tapers
        // from the help-droid side of the popup into the lower edge.
        const Vector2 corner = popup_rect_.position + Vector2(0, popup_rect_.size.y);
        PackedVector2Array connector;
        connector.push_back(corner);
        connector.push_back(corner + Vector2(148.0F, 0) * scale);
        connector.push_back(corner + Vector2(-11.0F, 19.0F) * scale);
        PackedColorArray connector_colours;
        connector_colours.push_back(Color(127.0F / 255.0F, 244.0F / 255.0F, 1, alpha));
        connector_colours.push_back(Color(87.0F / 255.0F, 114.0F / 255.0F, 197.0F / 255.0F, 0));
        connector_colours.push_back(Color(122.0F / 255.0F, 241.0F / 255.0F, 249.0F / 255.0F, alpha));
        owner.draw_polygon(connector, connector_colours);
        draw_asset(popup_content_.population ? "e_topbar.tga" : "e_topbar2.tga", popup_title_rect_);
        if (!popup_content_.icon_back.empty()) {
            const auto tint = popup_content_.icon_back_tint.value_or(data::ui::Rgba8{255, 255, 255, 255});
            draw_asset(popup_content_.icon_back, popup_back_rect_, Color(tint.r / 255.0F, tint.g / 255.0F, tint.b / 255.0F, tint.a / 255.0F));
        }
        draw_asset(popup_content_.icon, popup_portrait_rect_);
        for (std::size_t index = 0; index < popup_cell_rects_.size() && index < popup_content_.cells.size(); ++index)
            draw_asset(popup_content_.cells[index].icon, popup_cell_rects_[index]);
        for (std::size_t index = 0; index < popup_ability_rects_.size(); ++index)
            draw_asset(popup_content_.ability_icons[index], popup_ability_rects_[index]);
        for (std::size_t column = 0; column < 2; ++column) {
            const auto& icons = column == 0 ? popup_content_.good_against_icons : popup_content_.vulnerable_to_icons;
            if (popup_content_.good_against_icons.empty() && popup_content_.vulnerable_to_icons.empty()) continue;
            draw_asset("e_against_frame.tga", popup_matchup_rects_[column],
                column == 0 ? Color(0, 192.0F / 255.0F, 0, 1) : Color(192.0F / 255.0F, 0, 0, 1));
            for (std::size_t slot = 0; slot < 3; ++slot) {
                const auto& box = popup_icon_rects_[column * 3 + slot];
                if (slot < icons.size()) draw_asset(icons[slot], box);
            }
        }
    }
    owner.draw_set_transform(Vector2{});
}
EawrEncyclopediaPopup::EawrEncyclopediaPopup() {
    set_mouse_filter(MOUSE_FILTER_STOP);
    set_focus_mode(FOCUS_NONE);
}
void EawrEncyclopediaPopup::setup(EncyclopediaPopupStyle style) {
    style_ = std::move(style);
    renderer_.initialize(*this);
}
void EawrEncyclopediaPopup::update(const model::UnitCardLooks& content, const EncyclopediaPopupStatus& status,
    const Vector2& bottom_left, const model::ReferenceSpace& space, const float scale,
    const std::optional<Rect2> bounds) {
    status_ = status;
    scale_ = scale;
    renderer_.update(style_, content, status, bottom_left, space, scale);
    if (bounds) {
        const auto& rect = renderer_.popup_rect_;
        const Vector2 position(
            std::clamp(rect.position.x, bounds->position.x, std::max(bounds->position.x, bounds->get_end().x - rect.size.x)),
            std::clamp(rect.position.y, bounds->position.y, std::max(bounds->position.y, bounds->get_end().y - rect.size.y)));
        if (position != rect.position)
            renderer_.update(style_, content, status, position + Vector2(0, rect.size.y), space, scale);
    }
    set_position(renderer_.popup_rect_.position);
    set_size(renderer_.popup_rect_.size);
    renderer_.popup_text_owner_->set_position(-get_position());
    queue_redraw();
}
void EawrEncyclopediaPopup::_draw() {
    renderer_.draw(*this, style_, scale_, 1.0F, get_position());
}

std::string EawrUnitCards::report_json() const {
    std::ostringstream output;
    output << "{\"slots\": " << setup_.slots.size() << ", \"borders\": " << setup_.borders.size()
           << ", \"build_card_batches\": " << build_card_batches_ << ", \"build_state_updates\": " << build_state_updates_
           << ", \"popup_content_builds\": " << popup_content_builds_
           << ", \"clicks\": " << clicks_ << ", \"double_clicks\": " << double_clicks_ << ", \"drawn\": [";
    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const Card& card = cards_[index];
        std::string icon;
        const Ref<Texture2D> texture = portrait(card.type, &icon);
        output << (index ? ", " : "") << "{\"slot\": " << card.slot << ", \"type\": " << json_space_controls_with_newline(card.type)
               << ", \"icon\": " << json_space_controls_with_newline(icon) << ", \"icon_drawn\": " << (texture.is_valid() ? "true" : "false")
               << ", \"count\": " << card.count << ", \"stacked\": " << (card.stacked ? "true" : "false")
               << ", \"health_level\": " << card.health_level;
        if (card.price) {
            output << ", \"price\": " << *card.price << ", \"disabled\": " << (card.disabled ? "true" : "false");
            if (card.build_frames) output << ", \"build_frames\": " << *card.build_frames;
            output << ", \"disabled_reason\": " << json_space_controls_with_newline(card.disabled_reason);
        }
        if (const auto rect = slot_rect(card.slot)) output << ", \"rect\": " << rect_json(*rect);
        if (!card.stacked && !card.price && card.slot < setup_.slots.size()) {
            const auto report_bar = [&](const char* key, const model::HudBar& bar) {
                if (bar.overlay.empty() || !setup_.texture) return;
                const auto texture = setup_.texture(bar.overlay.back());
                if (texture.is_null()) return;
                const assets::Vec2f centre{bar.origin.x + bar.offset.x, bar.origin.y + bar.offset.y};
                output << ", " << json_space_controls_with_newline(key) << ": "
                       << rect_json(aligned_bar(screen(centred(centre, texture->get_size(), bar.scale))));
            };
            const auto& look = setup_.slots[card.slot];
            if (look.health) report_bar("health_rect", *look.health);
            if (look.shield && card.shield) report_bar("shield_rect", *look.shield);
        }
        output << "}";
    }
    output << "], \"slot_rects\": [";
    for (std::size_t slot = 0; slot < setup_.slots.size(); ++slot) {
        output << (slot ? ", " : "") << rect_json(slot_rect(slot).value_or(Rect2()));
    }
    output << "], \"tooltip\": ";
    if (tooltip_shown_ && shown_card_) {
        const Card* card = &*shown_card_;
        String name = shown_external_ ? String::utf8(popup_.popup_content_.name.c_str()) : setup_.name ? setup_.name(card->type) : String();
        if (!card->disabled_reason.empty()) name += "\n" + String(card->disabled_reason.c_str());
        const CharString utf8 = name.utf8();
        output << "{\"slot\": " << card->slot << ", \"external\": " << (shown_external_ ? "true" : "false")
               << ", \"alpha\": " << timing_.alpha << ", \"text\": " << json_space_controls_with_newline(std::string(utf8.get_data(), utf8.length()))
               << ", \"rect\": " << rect_json(tooltip_rect_);
        if (popup_.popup_rect_.has_area()) {
            output << ", \"description\": " << json_space_controls_with_newline(popup_.popup_content_.description)
                   << ", \"unit_class\": " << json_space_controls_with_newline(popup_.popup_content_.unit_class)
                   << ", \"population\": " << popup_.popup_content_.population
                   << ", \"title_rect\": " << rect_json(popup_.popup_name_rect_)
                   << ", \"portrait_rect\": " << rect_json(popup_.popup_portrait_rect_)
                   << ", \"cost_font\": " << json_space_controls_with_newline(setup_.popup_cost_font.is_valid()
                       ? std::string(setup_.popup_cost_font->get_font_name().utf8().get_data()) : std::string());
            const auto icons = [&](const char* key, const std::vector<std::string>& entries, const std::size_t column) {
                output << ", " << json_space_controls_with_newline(key) << ": [";
                for (std::size_t slot = 0; slot < entries.size(); ++slot) {
                    const auto texture = entries[slot].empty() || !setup_.texture ? Ref<Texture2D>() : setup_.texture(entries[slot]);
                    output << (slot ? ", " : "") << "{\"icon\": " << json_space_controls_with_newline(entries[slot])
                           << ", \"icon_drawn\": " << (texture.is_valid() ? "true" : "false")
                           << ", \"rect\": " << rect_json(popup_.popup_icon_rects_[column * 3 + slot]) << "}";
                }
                output << "]";
            };
            icons("good_against", popup_.popup_content_.good_against_icons, 0);
            icons("vulnerable_to", popup_.popup_content_.vulnerable_to_icons, 1);
            output << ", \"ability_icons\": [";
            for (std::size_t index = 0; index < popup_.popup_content_.ability_icons.size(); ++index)
                output << (index ? ", " : "") << json_space_controls_with_newline(popup_.popup_content_.ability_icons[index]);
            output << "], \"icon_back\": " << json_space_controls_with_newline(popup_.popup_content_.icon_back) << ", \"cells\": [";
            for (std::size_t index = 0; index < popup_.popup_content_.cells.size() && index < popup_.popup_cell_rects_.size(); ++index) {
                const auto& cell = popup_.popup_content_.cells[index];
                output << (index ? ", " : "") << "{\"icon\": " << json_space_controls_with_newline(cell.icon) << ", \"count\": " << cell.count
                       << ", \"rect\": " << rect_json(popup_.popup_cell_rects_[index]) << "}";
            }
            output << ']';
        }
        output << "}";
    } else {
        output << "null";
    }
    output << "}";
    return output.str();
}

void fill_encyclopedia_popup(EncyclopediaPopupStyle& setup, const data::ui::CommandBarCatalog& catalog,
    const vfs::Vfs& filesystem, FontProvider& fonts, const std::string_view language,
    const data::ui::TextDatabase* text, std::vector<core::Diagnostic>& diagnostics) {
    const auto colour = [](const data::ui::Rgba8& value) {
        return Color(value.r / 255.0F, value.g / 255.0F, value.b / 255.0F, value.a / 255.0F);
    };
    const auto face = [&](const data::ui::CommandBarComponent& component, const std::int32_t points) {
        // CARD-19: command-bar text requests bold independently of Font_Name.
        return fonts.font(fonts.resolve({std::string(component.text(data::ui::Field::font_name)), points}, language), true);
    };
    if (const auto* back = catalog.find("encyclopedia_back")) {
        setup.tooltip_size = back->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
        const auto backgrounds = back->list(data::ui::Field::blank_texture_name);
        if (!backgrounds.empty()) setup.popup_background = backgrounds.front();
        const auto frames = back->list(data::ui::Field::icon_alternate_texture_name);
        // CARD-19: these are the edge and enlarged glow, for either faction.
        if (!frames.empty()) setup.popup_frame = frames[0];
        if (frames.size() > 1) setup.popup_glow = frames[1];
    }
    if (const auto* body = catalog.find("encyclopedia_text")) {
        setup.tooltip_point_size = body->integer(data::ui::Field::font_point_size).value_or(0);
        setup.popup_body_points = setup.tooltip_point_size;
        setup.popup_body_colour = colour(body->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{192, 192, 192, 200}));
        setup.popup_body_font = face(*body, setup.popup_body_points);
        if (const auto size = body->vec2(data::ui::Field::size)) setup.tooltip_size.y = size->y;
    }
    if (const auto* header = catalog.find("encyclopedia_header_text")) {
        setup.popup_header_points = header->integer(data::ui::Field::font_point_size).value_or(0);
        setup.popup_header_font = face(*header, setup.popup_header_points);
        setup.popup_header_size = header->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
    }
    if (const auto* cost = catalog.find("encyclopedia_cost_text")) {
        setup.popup_cost_points = cost->integer(data::ui::Field::font_point_size).value_or(0);
        setup.popup_cost_colour = colour(cost->color(data::ui::Field::text_color).value_or(data::ui::Rgba8{}));
        setup.popup_cost_font = face(*cost, setup.popup_cost_points);
    }
    if (const auto* icon = catalog.find("encyclopedia_icon")) {
        setup.popup_icon_scale = icon->vec2(data::ui::Field::size).value_or(data::ui::Vec2{});
        // WGUI-166: the fleet cells' xN caption is this component's text.
        setup.popup_cell_points = icon->integer(data::ui::Field::font_point_size).value_or(0);
        setup.popup_cell_font = fonts.font(fonts.resolve({std::string(icon->text(data::ui::Field::font_name)), setup.popup_cell_points}, language));
        setup.popup_cell_outline = icon->flag(data::ui::Field::text_outline);
        setup.popup_cell_offset = icon->vec2(data::ui::Field::text_offset).value_or(data::ui::Vec2{});
    }
    // CARD-19: timing and header offsets share matching, validation and warnings.
    const auto constants = model::encyclopedia_constants(filesystem);
    for (const auto& diagnostic : constants.diagnostics) diagnostics.push_back(diagnostic);
    setup.popup_population_offset = constants.population_offset;
    setup.popup_name_offset = constants.name_offset;
    setup.popup_cost_offset = constants.cost_offset;
    setup.popup_icon_x_offset = constants.icon_x_offset;
    setup.popup_icon_y_offset = constants.icon_y_offset;
    setup.popup_class_y_offset = constants.class_y_offset;
    const auto popup_text = [text](const std::string_view key) {
        const auto* entry = text ? text->find(key) : nullptr;
        return entry ? String::utf8(data::ui::to_utf8(entry->value).c_str()) : String();
    };
    setup.good_against_text = popup_text("TEXT_GOOD_AGAINST");
    setup.vulnerable_to_text = popup_text("TEXT_VULNERABLE_TO");
    // GameConstants.xml Encyclopedia_Delay: 750 ms before a hovered card opens its encyclopedia.
    setup.timing = constants.timing;
}

} // namespace eawr::presentation::godot_backend
