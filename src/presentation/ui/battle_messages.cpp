#include "eawr/presentation/ui/battle_messages.hpp"
#include "eawr/data/tag_trace.hpp"

#include <array>
#include <charconv>
#include <optional>

namespace eawr::presentation::ui {
namespace {

constexpr std::string_view constants_path = "data/xml/gameconstants.xml";

[[nodiscard]] std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

// The last child of that name wins, as later GameConstants entries do.
[[nodiscard]] const data::XmlNode* last_child(const data::XmlNode& root, const std::string_view name) noexcept {
    const data::XmlNode* found = nullptr;
    for (const data::XmlNode& child : root.children) {
        if (child.name == name) found = &child;
    }
    data::tag_trace::used(found);
    return found;
}

[[nodiscard]] std::optional<std::int32_t> integer(const std::string_view text) noexcept {
    const std::string_view value = trim(text);
    std::int32_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) return std::nullopt;
    return result;
}

// "r, g, b, a" with 0 to 255 each.
[[nodiscard]] std::optional<data::ui::Rgba8> colour(const std::string_view text) noexcept {
    std::array<std::uint8_t, 4> parts{};
    std::size_t start = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t comma = text.find(',', start);
        if ((comma == std::string_view::npos) != (index + 1 == parts.size())) return std::nullopt;
        const auto value = integer(text.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!value || *value < 0 || *value > 255) return std::nullopt;
        parts[index] = static_cast<std::uint8_t>(*value);
        start = comma + 1;
    }
    return data::ui::Rgba8{parts[0], parts[1], parts[2], parts[3]};
}

} // namespace

BattleMessageLooks battle_message_looks(const data::XmlNode& root) {
    BattleMessageLooks looks;
    const auto fallback = [&](const std::string_view tag, const std::string_view why) {
        core::Diagnostic diagnostic;
        diagnostic.code = std::string(diagnostic_codes::battle_message_constant);
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = std::string(tag) + " " + std::string(why) + "; the FoC value is used";
        diagnostic.logical_path = std::string(constants_path);
        looks.diagnostics.push_back(std::move(diagnostic));
    };
    if (const data::XmlNode* node = last_child(root, "Win_Lose_Message_Font"); node && !trim(node->raw_text).empty()) {
        looks.font = std::string(trim(node->raw_text));
    } else {
        fallback("Win_Lose_Message_Font", "is absent");
    }
    if (const data::XmlNode* node = last_child(root, "Win_Lose_Message_Font_Size")) {
        if (const auto size = integer(node->raw_text); size && *size > 0 && *size <= 200) looks.point_size = *size;
        else fallback("Win_Lose_Message_Font_Size", "is not a point size");
    } else {
        fallback("Win_Lose_Message_Font_Size", "is absent");
    }
    const std::array<std::pair<std::string_view, data::ui::Rgba8*>, 3> colours{
        {{"Win_Message_Color", &looks.win}, {"Lose_Message_Color", &looks.lose},
         {"Battle_Pending_Message_Color", &looks.pending}}};
    for (const auto& [tag, target] : colours) {
        const data::XmlNode* node = last_child(root, tag);
        if (node == nullptr) {
            fallback(tag, "is absent");
        } else if (const auto parsed = colour(node->raw_text)) {
            *target = *parsed;
        } else {
            fallback(tag, "is not r, g, b, a");
        }
    }
    return looks;
}

BattleMessageLooks battle_message_looks(const vfs::Vfs& filesystem) {
    auto document = data::load_document(filesystem, constants_path);
    if (!document) {
        BattleMessageLooks looks;
        core::Diagnostic diagnostic = document.error();
        diagnostic.code = std::string(diagnostic_codes::battle_message_constant);
        diagnostic.severity = core::Severity::warning;
        diagnostic.message = std::string(constants_path) + " cannot be read (" + document.error().message
            + "); the FoC values are used";
        looks.diagnostics.push_back(std::move(diagnostic));
        return looks;
    }
    return battle_message_looks(document.value().root);
}

std::string_view battle_message_key(const BattleResult result) noexcept {
    return result == BattleResult::victory ? "TEXT_WIN_TACTICAL" : "TEXT_LOSE_TACTICAL";
}

std::string_view battle_end_title_key(const BattleResult result) noexcept {
    return result == BattleResult::victory ? "TEXT_VICTORY" : "TEXT_DEFEAT";
}

MessagePlacement battle_message_placement(const double screen_width, const double screen_height,
                                          const double text_width) noexcept {
    return {screen_width * 0.5 - text_width * 0.5, screen_height * 0.4};
}

} // namespace eawr::presentation::ui
