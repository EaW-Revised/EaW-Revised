#include "unit_cards_view.hpp"

#include "eawr/presentation/ui/fonts.hpp"

#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/time.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

// Update_Tactical_Selections: a type without Icon_Name shows this portrait.
constexpr const char* fallback_portrait = "i_button_temporary.tga";
// The encyclopedia popup's header line in shell units, measured on a 1280x720 retail still.
constexpr data::ui::ReferenceRect encyclopedia_header{62.0F, 471.5F, 265.0F, 19.0F};

[[nodiscard]] Rect2 rect2(const model::PixelRect& rect) {
    return Rect2(static_cast<float>(rect.x), static_cast<float>(rect.y), static_cast<float>(rect.width),
                 static_cast<float>(rect.height));
}

[[nodiscard]] std::string json(const std::string_view value) {
    std::string out{"\""};
    for (const char character : value) {
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

EawrUnitCards::~EawrUnitCards() = default;

void EawrUnitCards::setup(Setup setup) {
    setup_ = std::move(setup);
    counts_.clear();
    for (std::size_t index = 0; index < setup_.slots.size(); ++index) counts_.push_back(std::make_unique<KitText>());
    set_process(true);
    queue_redraw();
}

void EawrUnitCards::show(std::vector<Card> cards, std::vector<model::CardBorder> borders) {
    const auto same_card = [](const Card& a, const Card& b) {
        return a.slot == b.slot && a.type == b.type && a.count == b.count && a.stacked == b.stacked
            && a.health_level == b.health_level && a.shield == b.shield;
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
        tooltip_shown_ = false;
    }
    update_text();
    queue_redraw();
}

Rect2 EawrUnitCards::screen(const data::ui::ReferenceRect& rect) const {
    return rect2(model::shell_to_screen(rect, setup_.placement()));
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
        if (tooltip_shown_) {
            tooltip_shown_ = false;
            update_text();
            queue_redraw();
        }
    } else if (what == NOTIFICATION_PROCESS) {
        if (get_size() != laid_out_) {
            laid_out_ = get_size();
            update_text();
            queue_redraw();
        }
        if (!hovered_ || tooltip_shown_) return;
        const bool over_card = std::any_of(cards_.begin(), cards_.end(), [&](const Card& card) { return card.slot == *hovered_; });
        if (!over_card) return;
        if (now() - hovered_since_ >= setup_.hover_delay_seconds) {
            tooltip_shown_ = true;
            update_text();
            queue_redraw();
        }
    }
}

void EawrUnitCards::update_text() {
    if (!setup_.placement || !setup_.space) return;
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
        // Text 2 sits centred on the bone plus Text_Offset2; a two-line-high box keeps it centred.
        const assets::Vec2f at{look.card.origin.x + look.count_offset.x, look.card.origin.y + look.count_offset.y};
        const Rect2 box = screen({at.x - 25.0F, at.y - 6.0F, 50.0F, 12.0F});
        counts_[slot]->draw(*this, style, text, box, HORIZONTAL_ALIGNMENT_CENTER, true, false,
                            static_cast<float>(shell.scale), 1.0F);
    }
    String name;
    tooltip_rect_ = Rect2();
    if (tooltip_shown_ && hovered_) {
        const auto card = std::find_if(cards_.begin(), cards_.end(), [&](const Card& shown) { return shown.slot == *hovered_; });
        if (card != cards_.end() && setup_.name) {
            name = setup_.name(card->type);
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
        }
    }
    tooltip_.draw(*this, style, name, tooltip_rect_, HORIZONTAL_ALIGNMENT_CENTER, true, false, 1.0F, 1.0F);
}

void EawrUnitCards::draw_bar(const model::HudBar& bar, const std::int32_t level, const double percent) {
    if (!setup_.texture) return;
    const assets::Vec2f centre{bar.origin.x + bar.offset.x, bar.origin.y + bar.offset.y};
    if (!bar.back.empty()) {
        const Ref<Texture2D> back = setup_.texture(bar.back[std::min<std::size_t>(bar.back.size() - 1, bar.max_level)]);
        if (back.is_valid()) draw_texture_rect(back, screen(centred(centre, back->get_size(), bar.scale)), false);
    }
    if (bar.overlay.empty()) return;
    const std::size_t index = std::min<std::size_t>(bar.overlay.size() - 1, static_cast<std::size_t>(std::max(0, level)));
    const Ref<Texture2D> overlay = setup_.texture(bar.overlay[index]);
    if (overlay.is_null()) return;
    const Vector2 texels = overlay->get_size();
    data::ui::ReferenceRect quad = centred(centre, texels, bar.scale);
    if (bar.smooth) {
        // The debug build's bar component (CARD-4): the overlay narrows to the percent and keeps its left edge.
        quad.width *= static_cast<float>(std::clamp(percent, 0.0, 1.0));
    }
    if (quad.width <= 0.0F) return;
    draw_texture_rect(overlay, screen(quad), false);
}

void EawrUnitCards::_draw() {
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
            const Color tint(look.colour.r / 255.0F, look.colour.g / 255.0F, look.colour.b / 255.0F, look.colour.a / 255.0F);
            draw_texture_rect(texture, screen(model::button_quad(look.card, size.x, size.y)), false, tint);
        }
        if (card.stacked) continue; // a stacked card hides its bars
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
    if (tooltip_rect_.has_area()) draw_rect(tooltip_rect_, Color(0.0F, 0.0F, 0.0F, 0.75F));
}

std::string EawrUnitCards::report_json() const {
    std::ostringstream output;
    output << "{\"slots\": " << setup_.slots.size() << ", \"borders\": " << setup_.borders.size()
           << ", \"clicks\": " << clicks_ << ", \"double_clicks\": " << double_clicks_ << ", \"drawn\": [";
    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const Card& card = cards_[index];
        std::string icon;
        const Ref<Texture2D> texture = portrait(card.type, &icon);
        output << (index ? ", " : "") << "{\"slot\": " << card.slot << ", \"type\": " << json(card.type)
               << ", \"icon\": " << json(icon) << ", \"icon_drawn\": " << (texture.is_valid() ? "true" : "false")
               << ", \"count\": " << card.count << ", \"stacked\": " << (card.stacked ? "true" : "false")
               << ", \"health_level\": " << card.health_level;
        if (const auto rect = slot_rect(card.slot)) output << ", \"rect\": " << rect_json(*rect);
        output << "}";
    }
    output << "], \"slot_rects\": [";
    for (std::size_t slot = 0; slot < setup_.slots.size(); ++slot) {
        output << (slot ? ", " : "") << rect_json(slot_rect(slot).value_or(Rect2()));
    }
    output << "], \"tooltip\": ";
    if (tooltip_shown_ && hovered_) {
        const auto card = std::find_if(cards_.begin(), cards_.end(), [&](const Card& shown) { return shown.slot == *hovered_; });
        const String name = card != cards_.end() && setup_.name ? setup_.name(card->type) : String();
        const CharString utf8 = name.utf8();
        output << "{\"slot\": " << *hovered_ << ", \"text\": " << json(std::string(utf8.get_data(), utf8.length()))
               << ", \"rect\": " << rect_json(tooltip_rect_) << "}";
    } else {
        output << "null";
    }
    output << "}";
    return output.str();
}

} // namespace eawr::presentation::godot_backend
