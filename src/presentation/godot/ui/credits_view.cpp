#include "ui/credits_view.hpp"

#include "ui/movie_player.hpp"
#include "ui/theme_builder.hpp"

#include "eawr/assets/assets.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"

#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

using namespace godot;

namespace eawr::presentation::godot_backend {
namespace {

namespace model = presentation::ui;

// The 768-unit-high reference screen the font sizes are authored for (UI-L1).
constexpr float reference_height = 768.0F;
constexpr std::string_view constants_path = "data/xml/gameconstants.xml";
constexpr std::string_view texture_directory = "Data/Art/Textures/";

String godot_text(const std::string& value) {
    return String::utf8(value.c_str(), static_cast<int64_t>(value.size()));
}

Color colour(const model::CreditsColour& value) { return Color(value.red, value.green, value.blue, 1.0F); }

} // namespace

void EawrCreditsScreen::_bind_methods() { ADD_SIGNAL(MethodInfo("closed")); }

std::size_t EawrCreditsScreen::logos_loaded() const noexcept {
    return static_cast<std::size_t>(std::count_if(logos_.begin(), logos_.end(),
                                                   [](const TextureRect* logo) { return logo != nullptr; }));
}

bool EawrCreditsScreen::show(Node& parent, const CreditsContext& context) {
    if (context.filesystem == nullptr || context.fonts == nullptr) {
        failure_ = "the credits screen has no data or fonts";
        return false;
    }
    size_ = context.viewport_size;
    fonts_ = context.fonts;
    language_ = context.language;

    set_layer(layer_index);
    set_name("Credits");
    canvas_ = memnew(Control);
    canvas_->set_position(Vector2());
    canvas_->set_size(size_);
    canvas_->set_mouse_filter(Control::MOUSE_FILTER_STOP); // a click skips, and reaches nothing below
    add_child(canvas_);
    auto* backdrop = memnew(ColorRect);
    backdrop->set_color(Color(0.0F, 0.0F, 0.0F, 1.0F));
    backdrop->set_size(size_);
    backdrop->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
    canvas_->add_child(backdrop);
    // In the tree before the movie attaches: a player only starts playing inside it.
    parent.add_child(this);

    load_movie(context);
    load_text(context);
    if (!roll_ && movie_player_ == nullptr) {
        failure_ = "the credits have neither text to roll nor a movie to play";
        for (const std::string& note : notes_) failure_ += "; " + note;
        parent.remove_child(this);
        return false; // the caller frees the screen, and the canvas with it
    }
    if (roll_) {
        load_logos(context);
        // The logos' sizes are known now; the roll's end depends on them.
        roll_.emplace(*style_, lines_.size(), logo_heights_, size_.y);
        // The text goes on a layer of its own after the movie and the logos: canvas items the kit text
        // creates sit below the sibling nodes of their owner, so the owner must be above them.
        text_layer_ = memnew(Control);
        text_layer_->set_position(Vector2());
        text_layer_->set_size(size_);
        text_layer_->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        canvas_->add_child(text_layer_);
        text_layer_->connect("draw", callable_mp(this, &EawrCreditsScreen::on_draw));
    }
    if (movie_player_ != nullptr && !roll_) {
        // With no text the movie is the whole credits and its end closes them.
        movie_player_->set_loop(false);
        movie_player_->connect("finished", callable_mp(this, &EawrCreditsScreen::on_movie_finished));
    }
    set_process(true);
    set_process_input(true);
    if (text_layer_ != nullptr) text_layer_->queue_redraw();
    return true;
}

void EawrCreditsScreen::load_movie(const CreditsContext& context) {
    if (context.movie_cache.is_empty()) {
        notes_.push_back("no movie cache: the roll plays over a plain backdrop");
        return;
    }
    auto resolved = data::ui::resolve_hud_movie(*context.filesystem, model::credits_movie_name);
    if (!resolved) {
        notes_.push_back(core::format_diagnostic(resolved.error()));
        return;
    }
    movie_.emplace(std::move(resolved.value()));
    auto attached = attach_hud_movie(*canvas_, *movie_, context.movie_cache, Rect2(Vector2(), size_), true);
    if (!attached) {
        notes_.push_back(core::format_diagnostic(attached.error()));
        return;
    }
    movie_player_ = attached.value();
    // Behind the text: first child after the backdrop.
    canvas_->move_child(movie_player_, 1);
}

void EawrCreditsScreen::load_text(const CreditsContext& context) {
    auto constants = data::load_document(*context.filesystem, constants_path);
    if (!constants) {
        notes_.push_back(core::format_diagnostic(constants.error()));
        return;
    }
    auto style = model::parse_credits_style(constants.value().root);
    if (!style) {
        notes_.push_back(core::format_diagnostic(style.error()));
        return;
    }
    const auto path = model::credits_text_path(context.language);
    if (!path) {
        notes_.push_back(core::format_diagnostic(path.error()));
        return;
    }
    auto database = data::ui::load_text_database(*context.filesystem, path.value());
    if (!database) {
        notes_.push_back(core::format_diagnostic(database.error()));
        return;
    }
    lines_ = model::credits_lines(database.value());
    if (lines_.empty()) {
        notes_.push_back(path.value() + " has no lines");
        return;
    }
    style_.emplace(std::move(style.value()));
    face_ = style_->font;
    roll_.emplace(*style_, lines_.size(), logo_heights_, size_.y);
}

void EawrCreditsScreen::load_logos(const CreditsContext& context) {
    for (std::size_t index = 0; index < model::credits_logo_count; ++index) {
        const model::CreditsLogoStyle& logo = style_->logos[index];
        if (logo.texture.empty()) continue;
        auto texture = assets::load_texture(*context.filesystem, std::string(texture_directory) + logo.texture);
        if (!texture) {
            notes_.push_back(core::format_diagnostic(texture.error()));
            continue;
        }
        std::string failure;
        const Ref<Image> image = texture_image(texture.value(), failure);
        if (image.is_null() || image->get_width() <= 0 || image->get_height() <= 0) {
            notes_.push_back("logo " + logo.texture + ": " + failure);
            continue;
        }
        // The width is the data's fraction of the screen width; the height keeps the image's
        // proportions, scaled by the data's height value (CRD-07).
        const float aspect = static_cast<float>(image->get_height()) / static_cast<float>(image->get_width());
        logo_heights_[index] = logo.height * aspect * size_.x / size_.y;
        auto* rect = memnew(TextureRect);
        rect->set_texture(ImageTexture::create_from_image(image));
        rect->set_expand_mode(TextureRect::EXPAND_IGNORE_SIZE);
        rect->set_stretch_mode(TextureRect::STRETCH_SCALE);
        rect->set_mouse_filter(Control::MOUSE_FILTER_IGNORE);
        rect->set_size(Vector2(logo.width * size_.x, logo_heights_[index] * size_.y));
        rect->set_visible(false);
        canvas_->add_child(rect);
        logos_[index] = rect;
    }
}

void EawrCreditsScreen::on_draw() {
    if (!roll_ || !style_ || text_layer_ == nullptr) return;
    const float height = size_.y;
    const float scale = height / reference_height;
    const int size = std::max(1, static_cast<int>(std::lround(static_cast<float>(style_->font_size) * scale)));
    if (font_.is_null() || font_size_ != size) {
        font_size_ = size;
        font_ = fonts_->font(fonts_->resolve(presentation::ui::FontRequest{face_, size}, language_));
    }
    const Ref<Font> font = font_;

    std::set<std::size_t> visible;
    for (std::size_t index = 0; index < lines_.size(); ++index) {
        const float y = roll_->line_y(index);
        if (y > 1.0F) break; // every later line is lower still
        if (y + roll_->line_height() < 0.0F || lines_[index].text.empty()) continue;
        const float alpha = roll_->alpha_at(y);
        if (alpha <= 0.0F) continue;
        visible.insert(index);
        KitTextStyle text_style;
        text_style.font = font;
        text_style.size = size;
        const bool header = lines_[index].header;
        text_style.top = colour(header ? style_->header_top : style_->top);
        text_style.bottom = colour(header ? style_->header_bottom : style_->bottom);
        auto& slot = text_[index];
        if (!slot) slot = std::make_unique<KitText>();
        slot->draw(*text_layer_, text_style, godot_text(lines_[index].text),
                   Rect2(0.0F, std::round(y * height), size_.x, std::round(roll_->line_height() * height)),
                   HORIZONTAL_ALIGNMENT_CENTER, true, false, scale, alpha);
    }
    for (auto it = text_.begin(); it != text_.end();) {
        it = visible.count(it->first) != 0 ? std::next(it) : text_.erase(it);
    }

    for (std::size_t index = 0; index < model::credits_logo_count; ++index) {
        TextureRect* logo = logos_[index];
        if (logo == nullptr) continue;
        const model::CreditsLogoStyle& logo_style = style_->logos[index];
        const float y = roll_->logo_y(index);
        const float alpha = roll_->alpha_at(y);
        // Left, centred and right (CRD-07).
        const float width = logo_style.width;
        const float x = index == 0 ? 0.0F : index == 1 ? 0.5F - width * 0.5F : 1.0F - width;
        logo->set_position(Vector2(std::round(x * size_.x), std::round(y * height)));
        logo->set_modulate(Color(1.0F, 1.0F, 1.0F, alpha));
        logo->set_visible(alpha > 0.0F && y < 1.0F && y + roll_->logo_height(index) > 0.0F);
    }
}

void EawrCreditsScreen::_process(const double delta) {
    if (finished_) return;
    elapsed_ += delta;
    if (!roll_) return;
    roll_->advance(delta);
    if (text_layer_ != nullptr) text_layer_->queue_redraw();
    if (roll_->done()) finish();
}

void EawrCreditsScreen::_input(const Ref<InputEvent>& event) {
    if (finished_) return;
    bool skipped = false;
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        skipped = key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_ESCAPE;
    } else if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr())) {
        skipped = button->is_pressed();
    }
    if (!skipped) return;
    if (Viewport* viewport = get_viewport()) viewport->set_input_as_handled();
    skip();
}

void EawrCreditsScreen::skip() { finish(); }

void EawrCreditsScreen::on_movie_finished() { finish(); }

void EawrCreditsScreen::finish() {
    if (finished_) return;
    finished_ = true;
    emit_signal("closed");
    queue_free();
}

void register_credits_classes() { GDREGISTER_CLASS(EawrCreditsScreen); }

} // namespace eawr::presentation::godot_backend
