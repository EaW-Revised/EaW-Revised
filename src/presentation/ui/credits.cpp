#include "eawr/presentation/ui/credits.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <utility>

namespace eawr::presentation::ui {
namespace {

core::Diagnostic problem(const std::string_view code, std::string message) {
    return core::Diagnostic{std::string(code), core::Severity::error, std::move(message), {}, {}, {}, {}};
}

std::string_view trimmed(std::string_view text) {
    return core::trim_view(text, core::xml_whitespace);
}

bool same_name(const std::string_view left, const std::string_view right) {
    return core::iequals(left, right);
}

// The last declaration of a tag wins, as for the other GameConstants values.
const data::XmlNode* find_tag(const data::XmlNode& root, const std::string_view name) {
    const data::XmlNode* found = nullptr;
    for (const data::XmlNode& child : root.children) {
        if (same_name(child.name, name)) found = &child;
    }
    return found;
}

bool parse_float(std::string_view text, float& value) {
    text = trimmed(text);
    if (text.empty()) return false;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} && end == text.data() + text.size() && std::isfinite(value);
}

class Reader final {
public:
    explicit Reader(const data::XmlNode& root) : root_(root) {}

    [[nodiscard]] const std::string& failure() const noexcept { return failure_; }

    std::string text(const std::string_view tag) {
        const data::XmlNode* node = find_tag(root_, tag);
        if (node == nullptr || trimmed(node->raw_text).empty()) {
            fail(tag, "is absent");
            return {};
        }
        return std::string(trimmed(node->raw_text));
    }

    float number(const std::string_view tag) {
        const data::XmlNode* node = find_tag(root_, tag);
        float value{};
        if (node == nullptr) {
            fail(tag, "is absent");
        } else if (!parse_float(node->raw_text, value)) {
            fail(tag, "is not a number");
        }
        return value;
    }

    CreditsColour colour(const std::string_view tag) {
        const data::XmlNode* node = find_tag(root_, tag);
        CreditsColour result;
        if (node == nullptr) {
            fail(tag, "is absent");
            return result;
        }
        std::array<float, 3> parts{};
        std::string_view rest = node->raw_text;
        for (std::size_t index = 0; index < parts.size(); ++index) {
            const auto comma = rest.find(',');
            const bool last = index + 1 == parts.size();
            if ((comma == std::string_view::npos) != last || !parse_float(rest.substr(0, comma), parts[index])) {
                fail(tag, "is not three comma-separated numbers");
                return result;
            }
            if (!last) rest.remove_prefix(comma + 1);
        }
        return CreditsColour{parts[0], parts[1], parts[2]};
    }

private:
    void fail(const std::string_view tag, const std::string_view what) {
        if (failure_.empty()) failure_ = std::string(tag) + " " + std::string(what);
    }

    const data::XmlNode& root_;
    std::string failure_;
};

float smooth_step(const float low, const float high, const float value) noexcept {
    if (high <= low) return value >= high ? 1.0F : 0.0F;
    const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

} // namespace

core::Result<CreditsStyle> parse_credits_style(const data::XmlNode& constants_root) {
    Reader reader(constants_root);
    CreditsStyle style;
    style.font = reader.text("Credits_Font");
    style.font_size = static_cast<int>(std::lround(reader.number("Credits_Font_Size")));
    style.spacing = reader.number("Credits_Spacing");
    style.margin = reader.number("Credits_Margin");
    style.scroll_rate = reader.number("Credits_Scroll_Rate");
    style.top = reader.colour("Credits_Top_Color");
    style.bottom = reader.colour("Credits_Bottom_Color");
    style.header_top = reader.colour("Credits_Header_Top_Color");
    style.header_bottom = reader.colour("Credits_Header_Bottom_Color");
    constexpr std::array<std::string_view, credits_logo_count> prefixes{"Credits_Logo_1_", "Credits_Logo_2_",
                                                                         "Credits_Logo_3_"};
    for (std::size_t index = 0; index < credits_logo_count; ++index) {
        const std::string prefix(prefixes[index]);
        CreditsLogoStyle& logo = style.logos[index];
        logo.texture = reader.text(prefix + "Name");
        logo.width = reader.number(prefix + "Width");
        logo.height = reader.number(prefix + "Height");
        logo.y_offset = reader.number(prefix + "Y_Offset");
    }
    if (!reader.failure().empty()) {
        return core::Result<CreditsStyle>::failure(problem(credits_codes::constants, reader.failure()));
    }
    if (style.font_size <= 0 || style.spacing <= 0.0F || style.scroll_rate >= 0.0F) {
        return core::Result<CreditsStyle>::failure(problem(credits_codes::constants,
            "the font size and line spacing must be positive and the scroll rate negative (up)"));
    }
    return core::Result<CreditsStyle>::success(std::move(style));
}

core::Result<std::string> credits_text_path(const std::string_view language) {
    auto master = data::ui::text_database_path(language);
    if (!master) return master;
    std::string path = std::move(master.value());
    const std::string_view from = "MasterTextFile_";
    path.replace(path.find(from), from.size(), "CreditsText_");
    return core::Result<std::string>::success(std::move(path));
}

std::vector<CreditsLine> credits_lines(const data::ui::TextDatabase& database) {
    std::vector<CreditsLine> lines;
    lines.reserve(database.entries.size());
    for (const data::ui::TextEntry& entry : database.entries) {
        CreditsLine line;
        line.header = entry.key.find(credits_header_key_part) != std::string::npos;
        line.text = data::ui::to_utf8(entry.value);
        if (line.text == credits_blank_value) line.text.clear();
        lines.push_back(std::move(line));
    }
    return lines;
}

CreditsRoll::CreditsRoll(const CreditsStyle& style, const std::size_t line_count,
                         const std::array<float, credits_logo_count>& logo_heights, const float screen_height)
    : style_(style), line_count_(line_count), logo_heights_(logo_heights),
      screen_height_(std::max(screen_height, 1.0F)) {
    // The pitch snaps down to whole pixels (CRD-04).
    pitch_ = std::floor(style.spacing * screen_height_) / screen_height_;
}

float CreditsRoll::scroll_distance() const noexcept {
    const double pixels = std::floor(std::fabs(static_cast<double>(style_.scroll_rate)) * screen_height_ * elapsed_);
    return static_cast<float>(pixels / screen_height_);
}

float CreditsRoll::line_y(const std::size_t index) const noexcept {
    // The first line starts one height down plus half a pixel (CRD-04).
    const float start = 1.0F + 0.5F / screen_height_;
    return start + static_cast<float>(index) * pitch_ - scroll_distance();
}

float CreditsRoll::logo_y(const std::size_t index) const noexcept {
    const float start = 1.0F + 0.5F / screen_height_;
    return start + static_cast<float>(line_count_) * pitch_ + style_.logos[index].y_offset - scroll_distance();
}

float CreditsRoll::alpha_at(const float y) const noexcept {
    const float margin = style_.margin;
    if (y < margin) return 0.0F;
    if (y < margin * 2.0F) return smooth_step(margin, margin * 2.0F, y);
    if (y < 1.0F - margin * 2.0F) return 1.0F;
    return 1.0F - smooth_step(1.0F - margin * 2.0F, 1.0F - margin, y);
}

bool CreditsRoll::done() const noexcept {
    for (std::size_t index = 0; index < line_count_; ++index) {
        if (line_y(index) + pitch_ > 0.0F) return false;
    }
    for (std::size_t index = 0; index < credits_logo_count; ++index) {
        if (logo_present(index) && logo_y(index) + logo_heights_[index] > 0.0F) return false;
    }
    return true;
}

} // namespace eawr::presentation::ui
