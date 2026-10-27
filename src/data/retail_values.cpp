#include "eawr/data/retail_values.hpp"
#include "eawr/core/text.hpp"

#include "eawr/data/tag_trace.hpp"
#include "xml_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace eawr::data::retail {
namespace {

[[nodiscard]] bool space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view stripped(std::string_view text) noexcept {
    return core::trim_view(text);
}

[[nodiscard]] bool digit(const char value) noexcept { return value >= '0' && value <= '9'; }

struct TagRule {
    std::string_view tag;
    ListRule rule;
};

// ML-26 corrects AU-82's squadron inheritance; spawned rows still append to the base.
constexpr std::array<TagRule, 5> list_rules{{
    {"Squadron_Units", ListRule::replace_by_variant_all},
    {"Starting_Spawned_Units_Tech_0", ListRule::append},
    {"Reserve_Spawned_Units_Tech_0", ListRule::append},
    {"Squadron_Offsets", ListRule::replace_in_variant},
    {"Fire_Inaccuracy_Distance", ListRule::replace_in_variant},
}};

// The non-placeholder occurrences of `tag` in one layer, in document order.
[[nodiscard]] std::vector<const XmlNode*> occurrences(const Definition& layer, const std::string_view tag) {
    std::vector<const XmlNode*> result;
    for (const auto& child : layer.root.children) {
        // A nested record has its value in child nodes, rather than scalar text.
        if (ascii_iequals(child.name, tag) && (!child.children.empty() || !is_placeholder(child.raw_text))) {
            result.push_back(&child);
        }
    }
    return result;
}

} // namespace

bool is_placeholder(const std::string_view text) noexcept {
    const auto value = stripped(text);
    return value.empty() || value == "TBD";
}

std::int32_t integer_constant(const std::string_view text) noexcept {
    std::size_t index = 0;
    while (index < text.size() && space(text[index])) ++index;
    bool negative = false;
    if (index < text.size() && (text[index] == '-' || text[index] == '+')) negative = text[index++] == '-';
    std::int64_t magnitude = 0;
    for (; index < text.size() && digit(text[index]); ++index) {
        magnitude = magnitude * 10 + (text[index] - '0');
        if (magnitude > (std::int64_t{1} << 32)) magnitude = std::int64_t{1} << 32; // saturates below
    }
    const std::int64_t value = negative ? -magnitude : magnitude;
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return static_cast<std::int32_t>(value);
}

std::optional<bool> boolean(const std::string_view text) noexcept {
    if (is_placeholder(text)) return std::nullopt;
    const auto value = stripped(text);
    const char first = value.front();
    return value == "1" || first == 'Y' || first == 'y' || first == 'T' || first == 't';
}

std::optional<LeadingNumber> leading_number(const std::string_view text) {
    std::string_view value = text;
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    std::size_t end = 0;
    if (end < value.size() && (value[end] == '+' || value[end] == '-')) ++end;
    std::size_t mantissa_digits = 0;
    const auto digits = [&] {
        while (end < value.size() && digit(value[end])) {
            ++end;
            ++mantissa_digits;
        }
    };
    digits();
    if (end < value.size() && value[end] == '.') {
        ++end;
        digits();
    }
    if (mantissa_digits == 0) return std::nullopt;
    // An exponent counts only when digits follow it, as in a C number parse.
    if (end < value.size() && (value[end] == 'e' || value[end] == 'E')) {
        std::size_t exponent = end + 1;
        if (exponent < value.size() && (value[exponent] == '+' || value[exponent] == '-')) ++exponent;
        if (exponent < value.size() && digit(value[exponent])) {
            while (exponent < value.size() && digit(value[exponent])) ++exponent;
            end = exponent;
        }
    }
    auto parsed = sim::math::Fixed::from_decimal(value.substr(0, end));
    if (!parsed) return std::nullopt;
    return LeadingNumber{parsed.value(), !stripped(value.substr(end)).empty()};
}

std::optional<FloatValue> float_value(const std::string_view text) {
    if (is_placeholder(text)) return std::nullopt;
    if (const auto number = leading_number(text)) return FloatValue{number->value, !number->trailing_text};
    // No representable leading number: find out whether there are no digits (0) or the magnitude is too large.
    std::string_view value = text;
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    bool negative = false;
    if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
        negative = value.front() == '-';
        value.remove_prefix(1);
    }
    const auto starts = [&](const std::string_view word) {
        return value.size() >= word.size() && ascii_iequals(value.substr(0, word.size()), word);
    };
    bool digits = starts("inf");
    for (std::size_t index = 0; !digits && index < value.size() && (digit(value[index]) || value[index] == '.'); ++index)
        digits = digit(value[index]);
    if (!digits) return FloatValue{sim::math::Fixed{}, false};
    const auto limit = std::numeric_limits<std::int64_t>::max();
    return FloatValue{sim::math::Fixed::from_raw(negative ? -limit : limit), false};
}

std::optional<IntegerValue> integer_value(const std::string_view text) {
    if (is_placeholder(text)) return std::nullopt;
    std::string_view value = text;
    while (!value.empty() && space(value.front())) value.remove_prefix(1);
    bool negative = false;
    if (!value.empty() && (value.front() == '+' || value.front() == '-')) {
        negative = value.front() == '-';
        value.remove_prefix(1);
    }
    std::int64_t magnitude = 0;
    std::size_t end = 0;
    bool saturated = false;
    while (end < value.size() && digit(value[end])) {
        magnitude = magnitude * 10 + (value[end++] - '0');
        if (magnitude > std::int64_t{1} << 31) {
            magnitude = std::int64_t{1} << 31;
            saturated = true;
        }
    }
    const auto low = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min());
    const auto high = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
    const auto signed_value = std::clamp(negative ? -magnitude : magnitude, low, high);
    saturated = saturated || signed_value != (negative ? -magnitude : magnitude);
    const bool exact = end > 0 && !saturated && stripped(value.substr(end)).empty();
    return IntegerValue{static_cast<std::int32_t>(signed_value), exact};
}

std::optional<ListRule> list_rule(const std::string_view tag) noexcept {
    for (const auto& item : list_rules) {
        if (ascii_iequals(item.tag, tag)) return item.rule;
    }
    return std::nullopt;
}

const XmlNode* single(const Layers layers, const std::string_view tag) {
    for (const auto* layer : layers) {
        if (layer == nullptr) continue;
        const auto found = occurrences(*layer, tag);
        if (found.empty()) continue;
        tag_trace::used(found.back());
        return found.back();
    }
    return nullptr;
}

std::vector<const XmlNode*> list(const Layers layers, const std::string_view tag, const ListRule rule) {
    std::vector<const XmlNode*> result;
    // Base first: the base type is the last layer, every earlier one a variant of the next.
    for (std::size_t index = layers.size(); index-- > 0;) {
        if (layers[index] == nullptr) continue;
        auto found = occurrences(*layers[index], tag);
        if (found.empty()) continue;
        const bool variant = index + 1 < layers.size();
        if (rule == ListRule::replace_in_variant && variant) {
            result.assign(1, found.back());
        } else if (rule == ListRule::replace_by_variant_all && variant) {
            result = std::move(found);
        } else {
            result.insert(result.end(), found.begin(), found.end());
        }
    }
    for (const auto* node : result) tag_trace::used(node);
    return result;
}

} // namespace eawr::data::retail
