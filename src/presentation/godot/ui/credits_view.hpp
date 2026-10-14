#pragma once

// The credits screen (MNU-07, docs/ui/menu-screens.md; rules in docs/behaviour/credits-roll.md): a
// CanvasLayer that plays the `Credits` movie full-screen and rolls the CreditsText lines and logos
// up over it, as the retail credits route does. It resolves the movie through the same movies.xml
// path as the HUD movies (data/ui/movie.hpp, movie_player.hpp) and draws the text with the font
// provider's faces through the kit's gradient text. A movie that cannot be played (not converted
// yet, or not in the data) leaves the roll over a plain backdrop, with the reason in notes().
//
// Esc or a click skips; the screen emits `closed` when the roll has scrolled off the top (or, with
// no text to roll, when the movie ends) and frees itself. The caller returns to the main menu.

#include "ui/font_provider.hpp"
#include "ui/kit.hpp"

#include "eawr/data/ui/movie.hpp"
#include "eawr/presentation/ui/credits.hpp"
#include "eawr/vfs/vfs.hpp"

#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/color_rect.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/texture_rect.hpp>
#include <godot_cpp/classes/video_stream_player.hpp>
#include <godot_cpp/variant/vector2.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

struct CreditsContext final {
    const vfs::Vfs* filesystem{};        // the effective data (mods included); outlives the screen
    FontProvider* fonts{};               // outlives the screen
    std::string language{"ENGLISH"};     // the text database's language word
    godot::String movie_cache;           // the player's converted-movie cache; empty plays no movie
    godot::Vector2 viewport_size;
};

class EawrCreditsScreen final : public godot::CanvasLayer {
    GDCLASS(EawrCreditsScreen, godot::CanvasLayer)

public:
    static constexpr int layer_index = 95; // over the menus, under the message box (100)

    EawrCreditsScreen() = default;

    // Loads the data and adds the screen under `parent`. A false return leaves it unopened with the
    // reason in failure() (no text to roll and no movie to play); the caller frees it.
    [[nodiscard]] bool show(godot::Node& parent, const CreditsContext& context);

    // Closes as a skip does: emits `closed` once and frees the screen.
    void skip();

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }
    // One line per thing the data could not give (the movie, the text, a logo).
    [[nodiscard]] const std::vector<std::string>& notes() const noexcept { return notes_; }
    [[nodiscard]] godot::VideoStreamPlayer* movie_player() const noexcept { return movie_player_; }
    [[nodiscard]] const std::optional<data::ui::HudMovie>& movie() const noexcept { return movie_; }
    [[nodiscard]] bool rolling() const noexcept { return roll_.has_value(); }
    [[nodiscard]] std::size_t line_count() const noexcept { return lines_.size(); }
    [[nodiscard]] std::size_t logos_loaded() const noexcept;
    [[nodiscard]] float scroll_moved() const noexcept { return roll_ ? roll_->scroll_distance() : 0.0F; }
    [[nodiscard]] double elapsed() const noexcept { return roll_ ? roll_->elapsed() : elapsed_; }
    [[nodiscard]] bool finished() const noexcept { return finished_; }

    void _process(double delta) override;
    void _input(const godot::Ref<godot::InputEvent>& event) override;

protected:
    static void _bind_methods();

private:
    void load_movie(const CreditsContext& context);
    void load_text(const CreditsContext& context);
    void load_logos(const CreditsContext& context);
    void on_draw();
    void on_movie_finished();
    void finish();

    godot::Vector2 size_;
    std::string failure_;
    std::vector<std::string> notes_;
    std::optional<data::ui::HudMovie> movie_;
    godot::VideoStreamPlayer* movie_player_{};
    godot::Control* canvas_{};
    godot::Control* text_layer_{}; // above the movie and the logos; the text items hang off it
    FontProvider* fonts_{};
    std::string face_;
    std::string language_;
    godot::Ref<godot::Font> font_;
    int font_size_{};
    std::optional<presentation::ui::CreditsStyle> style_;
    std::vector<presentation::ui::CreditsLine> lines_;
    std::array<godot::TextureRect*, presentation::ui::credits_logo_count> logos_{};
    std::array<float, presentation::ui::credits_logo_count> logo_heights_{};
    std::optional<presentation::ui::CreditsRoll> roll_;
    std::map<std::size_t, std::unique_ptr<KitText>> text_;
    double elapsed_{};
    bool finished_{};
};

void register_credits_classes();

} // namespace eawr::presentation::godot_backend
