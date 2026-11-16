#pragma once

#include "eawr/sim/galactic/story_goals.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eawr::viewer::galactic {

// The text a story plot puts on the HUD (#236 PR B; WST-47, WST-48): the objective list and the SCREEN_TEXT lines. The
// class only mirrors the presentation events the session published for this seat; it never reaches the simulation, and
// nothing here is saved or hashed (the objective list itself is story state the session saves, and it republishes the
// list after a load). It knows no text id or component name: the widget resolves ids through the text database.
class StoryTextState final {
public:
    // WST-48: the seconds default is 5; -1 (stock) keeps the line until a later SCREEN_TEXT removes it.
    static constexpr double default_seconds = 5.0;
    // A safety cap, not a rule of the original.
    static constexpr std::size_t max_lines = 16;

    enum class Style { plain, hint, system, task };
    struct Line {
        std::string text_id;
        double seconds_left{}; // negative: until removed
        bool teletype{true};
        std::optional<std::array<float, 3>> colour; // three authored numbers; the widget maps them to a colour
        Style style{Style::plain};
    };

    // One presentation event of this seat. Returns whether it was one of this class's (objectives or screen text).
    bool apply(const sim::galactic::story::PresentationEvent& event) {
        namespace st = sim::galactic::story;
        if (const auto* changed = std::get_if<st::ObjectiveChanged>(&event.payload)) { objective(*changed); return true; }
        const auto* request = std::get_if<st::ConsumerRequest>(&event.payload);
        if (!request || request->consumer != st::Consumer::text || request->reward_name != "SCREEN_TEXT") return false;
        screen_text(request->parameters);
        return true;
    }

    // Counts the timed lines down; a line that reaches zero leaves. A negative line stays until a removal.
    void advance(double seconds) {
        for (auto& line : lines_) {
            if (line.seconds_left <= 0.0) continue;
            line.seconds_left = std::max(0.0, line.seconds_left - std::max(0.0, seconds));
        }
        std::erase_if(lines_, [](const Line& line) { return line.seconds_left == 0.0; });
    }

    // A reader that fell behind the feed rebuilds the objective list from the session's state instead of from the events it
    // missed. The timed lines are local and transient; the ones that were lost stay lost.
    void resync(const sim::galactic::story::GoalsState& state) {
        objectives_ = state.objectives;
        visible_ = state.objectives_visible;
    }

    [[nodiscard]] const std::vector<sim::galactic::story::Objective>& objectives() const noexcept { return objectives_; }
    [[nodiscard]] bool objectives_visible() const noexcept { return visible_; }
    [[nodiscard]] const std::vector<Line>& lines() const noexcept { return lines_; }

private:
    void objective(const sim::galactic::story::ObjectiveChanged& changed) {
        namespace st = sim::galactic::story;
        visible_ = changed.visible;
        const auto first = [&](std::string_view identity) {
            return std::find_if(objectives_.begin(), objectives_.end(), [&](const auto& row) { return row.text_id == identity; });
        };
        switch (changed.action) {
        case st::ObjectiveAction::add:
            if (changed.value) {
                const auto size = static_cast<std::int64_t>(objectives_.size());
                const auto at = changed.index >= 0 && changed.index <= size ? changed.index : size;
                objectives_.insert(objectives_.begin() + at, *changed.value);
            }
            break;
        case st::ObjectiveAction::replace:
        case st::ObjectiveAction::complete:
        case st::ObjectiveAction::fail:
            if (const auto at = first(changed.identity); at != objectives_.end() && changed.value) *at = *changed.value;
            break;
        case st::ObjectiveAction::remove:
            if (const auto at = first(changed.identity); at != objectives_.end()) objectives_.erase(at);
            break;
        case st::ObjectiveAction::remove_all: objectives_.clear(); break;
        case st::ObjectiveAction::display: break;
        }
    }

    // WST-48 positions: 1 text id, 2 seconds (default 5, -1 until removed), 3 object type (the widget does not fill the
    // text variable: unverified), 4 any text removes the line, 5 teletype (default on, 0 off), 6 colour as three
    // space-separated numbers, 7 a style word (Hint, System, Task; case-insensitive).
    void screen_text(const std::vector<std::string>& parameters) {
        const auto at = [&](std::size_t slot) -> std::string_view { return slot < parameters.size() ? std::string_view(parameters[slot]) : std::string_view{}; };
        const auto id = at(0);
        if (id.empty()) return;
        if (!at(3).empty()) { // remove the line instead of showing it
            const auto found = std::find_if(lines_.begin(), lines_.end(), [&](const Line& line) { return line.text_id == id; });
            if (found != lines_.end()) lines_.erase(found);
            return;
        }
        Line line;
        line.text_id = std::string(id);
        line.seconds_left = number(at(1)).value_or(default_seconds);
        if (line.seconds_left == 0.0) line.seconds_left = default_seconds; // a zero would end the line at once and read as "none"
        if (const auto teletype = number(at(4))) line.teletype = *teletype != 0.0;
        line.colour = colour(at(5));
        line.style = style(at(6));
        if (lines_.size() >= max_lines) lines_.erase(lines_.begin());
        lines_.push_back(std::move(line));
    }

    [[nodiscard]] static std::optional<double> number(std::string_view text) {
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
        if (text.empty()) return std::nullopt;
        if (text.front() == '+') text.remove_prefix(1);
        double value{};
        const auto [stop, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || stop != text.data() + text.size()) return std::nullopt;
        return value;
    }
    [[nodiscard]] static std::optional<std::array<float, 3>> colour(std::string_view text) {
        std::array<float, 3> out{};
        for (auto& channel : out) {
            while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
            const auto end = text.find_first_of(" \t");
            const auto value = number(text.substr(0, end));
            if (!value) return std::nullopt;
            channel = static_cast<float>(*value);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end);
        }
        return out;
    }
    [[nodiscard]] static Style style(std::string_view word) {
        std::string lower(word);
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower == "hint") return Style::hint;
        if (lower == "system") return Style::system;
        if (lower == "task") return Style::task;
        return Style::plain;
    }

    std::vector<sim::galactic::story::Objective> objectives_;
    bool visible_{true};
    std::vector<Line> lines_;
};

} // namespace eawr::viewer::galactic
