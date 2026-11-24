#pragma once

#include "eawr/core/from_chars.hpp"

#include "eawr/core/text.hpp"
#include "eawr/data/tag_trace.hpp"
#include "eawr/data/xml.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace eawr::viewer::galactic {

[[nodiscard]] inline std::size_t wide_length(std::u32string_view text) {
    std::size_t result{};
    for (const auto c : text) result += c > 0xffff ? 2 : 1;
    return result;
}
[[nodiscard]] inline std::size_t wide_prefix(std::u32string_view text, std::size_t units) {
    std::size_t result{};
    for (const auto c : text) {
        const std::size_t width = c > 0xffff ? 2 : 1;
        if (units < width) break; // never display half a surrogate pair
        units -= width; ++result;
    }
    return result;
}

// WGP-197 / GFC-40: the authored rate is wide-character units per wall second.
// Missing/malformed authoring leaves the provider unavailable; zero is a valid rate.
[[nodiscard]] inline std::optional<float> text_reveal_rate(const data::XmlNode& constants) {
    const data::XmlNode* found{};
    for (const auto& child : constants.children)
        if (core::iequals(child.name, "Text_Reveal_Rate")) found = &child;
    if (!found) return std::nullopt;
    auto text = core::trim_view(found->raw_text, core::xml_whitespace);
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    float rate{};
    const auto [end, error] = eawr::core::from_chars(text.data(), text.data() + text.size(), rate);
    if (error != std::errc{} || end != text.data() + text.size() || !std::isfinite(rate)) return std::nullopt;
    data::tag_trace::used(found);
    return rate;
}

class TextReveal final {
public:
    struct Line final {
        std::size_t length{};
        float progress{}, stamp{};
    };

    void begin(std::span<const std::size_t> lengths, bool teletype, float now) {
        teletype_ = teletype;
        lines_.clear();
        for (const auto length : lengths) lines_.push_back({length, teletype ? 0.0F : static_cast<float>(length), 0.0F});
        if (!lines_.empty()) lines_.front().stamp = now;
    }
    void advance(float now, float rate) {
        if (!teletype_) return;
        for (std::size_t i = 0; i < lines_.size(); ++i) {
            auto& line = lines_[i];
            if (finished(line)) continue;
            if (i && line.stamp == 0.0F && !finished(lines_[i - 1])) continue;
            if (i && line.stamp == 0.0F) line.stamp = lines_[i - 1].stamp;
            line.progress = std::min(static_cast<float>(line.length), line.progress + rate * (now - line.stamp));
            line.stamp = now;
        }
    }
    [[nodiscard]] std::size_t visible(std::size_t index) const {
        if (!teletype_) return lines_.at(index).length;
        // GFC-40: the stored final length, and the separate 1023-unit display bound.
        const float value = std::clamp(lines_.at(index).progress, 0.0F,
            static_cast<float>(std::min<std::size_t>(lines_.at(index).length, 1023)));
        return static_cast<std::size_t>(value);
    }
    [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }

private:
    [[nodiscard]] static bool finished(const Line& line) { return line.progress >= static_cast<float>(line.length); }
    std::vector<Line> lines_;
    bool teletype_{};
};

} // namespace eawr::viewer::galactic
