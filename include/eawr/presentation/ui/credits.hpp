#pragma once

// The credits roll's model (MNU-07, docs/ui/menu-screens.md; rules CRD-01 to CRD-08 in
// docs/behaviour/credits-roll.md), engine-free. The roll is the text of CreditsText_<LANGUAGE>.dat
// scrolling up over the `Credits` movie, drawn with the GameConstants.xml Credits_* values, followed
// by up to three logo images, and it ends when everything has scrolled off the top. Every name,
// colour, size and rate comes from the loaded data, so a mod replaces them.
//
// Positions are fractions of the screen: y of the height (0 top, 1 bottom), x of the width.

#include "eawr/core/result.hpp"
#include "eawr/data/ui/text_database.hpp"
#include "eawr/data/xml.hpp"

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::presentation::ui {

namespace credits_codes {
inline constexpr std::string_view constants = "EAWR-UI-0901"; // a Credits_* value is missing or malformed
inline constexpr std::string_view text = "EAWR-UI-0902";      // the credits text could not be read
} // namespace credits_codes

// The movies.xml name the credits route plays (CRD-01): the debug build's route names it, and a mod
// replaces what it resolves to.
inline constexpr std::string_view credits_movie_name = "Credits";
// What a CreditsText key and value mean (CRD-03).
inline constexpr std::string_view credits_header_key_part = "HEADER";
inline constexpr std::string_view credits_blank_value = "[TBL]";
inline constexpr std::size_t credits_logo_count = 3;

struct CreditsColour final {
    float red{};
    float green{};
    float blue{};

    friend bool operator==(const CreditsColour&, const CreditsColour&) = default;
};

struct CreditsLogoStyle final {
    std::string texture; // a file name under Data/Art/Textures
    float width{};       // fraction of the screen width
    float height{};      // the height scale over the width-preserving aspect
    float y_offset{};    // below the last text line, fraction of the screen height
};

// The GameConstants.xml `Credits_*` values (CRD-02).
struct CreditsStyle final {
    std::string font;
    int font_size{};
    float spacing{};      // line pitch, fraction of the screen height
    float margin{};       // the fade band at the top and the bottom edge
    float scroll_rate{};  // screen heights per second; negative scrolls up
    CreditsColour top;
    CreditsColour bottom;
    CreditsColour header_top;
    CreditsColour header_bottom;
    std::array<CreditsLogoStyle, credits_logo_count> logos;
};

// Reads the values from the root of the effective GameConstants.xml. Fails with credits_codes::constants
// naming the first tag that is absent or not a number (or three comma-separated numbers).
[[nodiscard]] core::Result<CreditsStyle> parse_credits_style(const data::XmlNode& constants_root);

// Data/Text/CreditsText_<LANGUAGE>.dat for the language word of the master text database.
[[nodiscard]] core::Result<std::string> credits_text_path(std::string_view language);

struct CreditsLine final {
    std::string text; // UTF-8; empty for a blank line
    bool header{};
};

// One line per record in file order (CRD-03): the key part HEADER marks a header, the value [TBL] is a
// blank line.
[[nodiscard]] std::vector<CreditsLine> credits_lines(const data::ui::TextDatabase& database);

// Where each item is at a time into the roll. Items are the text lines, then the logos present.
class CreditsRoll final {
public:
    // `screen_height` in pixels (the roll moves in whole pixels and snaps the line pitch to them).
    // `logo_heights` holds each logo's drawn height as a fraction of the screen height, or a
    // non-positive value for a logo that is absent.
    CreditsRoll(const CreditsStyle& style, std::size_t line_count,
                const std::array<float, credits_logo_count>& logo_heights, float screen_height);

    void advance(double seconds) noexcept { elapsed_ += seconds; }
    void restart() noexcept { elapsed_ = 0.0; }
    [[nodiscard]] double elapsed() const noexcept { return elapsed_; }

    [[nodiscard]] std::size_t line_count() const noexcept { return line_count_; }
    // Top edge of a text line or logo now.
    [[nodiscard]] float line_y(std::size_t index) const noexcept;
    [[nodiscard]] float logo_y(std::size_t index) const noexcept;
    [[nodiscard]] bool logo_present(std::size_t index) const noexcept { return logo_heights_[index] > 0.0F; }
    [[nodiscard]] float line_height() const noexcept { return pitch_; }
    [[nodiscard]] float logo_height(std::size_t index) const noexcept { return logo_heights_[index]; }
    // Opacity at a top edge (CRD-05): zero above the margin, rising over the next margin, one down to
    // two margins above the bottom, falling to zero a margin above it.
    [[nodiscard]] float alpha_at(float y) const noexcept;
    // Every item's bottom edge is at or above the top of the screen (CRD-06).
    [[nodiscard]] bool done() const noexcept;
    // The scroll's whole-pixel offset at `elapsed` (CRD-04).
    [[nodiscard]] float scroll_distance() const noexcept;

private:
    CreditsStyle style_;
    std::size_t line_count_{};
    std::array<float, credits_logo_count> logo_heights_{};
    float screen_height_{};
    float pitch_{};
    double elapsed_{};
};

} // namespace eawr::presentation::ui
