#include "eawr/presentation/ui/loading_screen.hpp"
#include "eawr/core/text.hpp"

#include <algorithm>
#include <utility>

namespace eawr::presentation::ui {
namespace {

bool equal_name(const std::string_view a, const std::string_view b) {
    return core::iequals(a, b);
}

std::string trimmed(const std::string_view value) {
    return core::trim(value, core::xml_whitespace);
}

core::Diagnostic warning(const std::string_view code, std::string message) {
    core::Diagnostic out;
    out.code = std::string(code);
    out.severity = core::Severity::warning;
    out.message = std::move(message);
    return out;
}

} // namespace

std::string loading_weather_icon(const std::string_view encyclopedia_icon) {
    constexpr std::string_view prefix = "i_pa";
    if (encyclopedia_icon.substr(0, prefix.size()) != prefix) return std::string(encyclopedia_icon);
    return "load" + std::string(encyclopedia_icon.substr(prefix.size()));
}

std::string loading_map_icon_path(const std::string_view map_path) {
    const auto separator = map_path.find_last_of("/\\");
    std::string name(separator == std::string_view::npos ? map_path : map_path.substr(separator + 1U));
    if (name.empty()) return {};
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos) name.resize(dot);
    if (name.empty()) return {};
    return "data/art/textures/" + name + ".dds";
}

std::string loading_mission_text_key(const std::string_view prefix, const std::string_view dialog_map) {
    std::string map = core::to_upper(dialog_map);
    if (const auto extension = map.find(".TED"); extension != std::string::npos) map.resize(extension);
    return std::string(prefix) + map;
}

std::vector<std::string> loading_wrap_lines(const std::string_view text, const double max_width,
                                            const std::function<double(std::string_view)>& measure) {
    std::vector<std::string> lines;
    std::size_t paragraph = 0;
    while (paragraph <= text.size()) {
        const auto end = text.find('\n', paragraph);
        std::string_view body = text.substr(paragraph, end == std::string_view::npos ? std::string_view::npos : end - paragraph);
        if (!body.empty() && body.back() == '\r') body.remove_suffix(1);
        std::string line;
        std::size_t word = 0;
        bool first = true;
        while (word <= body.size()) {
            const auto space = body.find(' ', word);
            const std::string_view token = body.substr(word, space == std::string_view::npos ? std::string_view::npos : space - word);
            std::string candidate = first ? std::string(token) : line + " " + std::string(token);
            if (first || measure(candidate) <= max_width) {
                line = std::move(candidate);
            } else {
                lines.push_back(std::move(line));
                line = std::string(token);
            }
            first = false;
            if (space == std::string_view::npos) break;
            word = space + 1U;
        }
        lines.push_back(std::move(line));
        if (end == std::string_view::npos) break;
        paragraph = end + 1U;
    }
    return lines;
}

LoadingScreenContent loading_screen_content(const LoadingScreenInput& input) {
    LoadingScreenContent out;
    const bool mission = input.campaign && input.mission != LoadingMission::ordinary;
    if (input.campaign) out.dialog = input.battle == LoadingBattle::space
        ? (mission ? "IDD_SPACE_MISSION_LOAD_DIALOG" : "IDD_SPACE_BATTLE_LOAD_DIALOG")
        : (mission ? "IDD_LAND_MISSION_LOAD_DIALOG" : "IDD_LAND_BATTLE_LOAD_DIALOG");
    out.overlay_icon = "load_overlay.tga";
    const auto lookup = [&](const std::string_view key) -> std::string {
        if (key.empty()) return {};
        if (input.text == nullptr) return std::string(key);
        return data::ui::to_utf8(input.text->text(key));
    };
    if (mission) {
        if (input.mission_map && !input.mission_map->empty()) {
            out.mission_heading = lookup(loading_mission_text_key("MT", *input.mission_map));
            out.mission_body = lookup(loading_mission_text_key("MB", *input.mission_map));
        } else out.diagnostics.push_back(warning(diagnostic_codes::loading_text,
            "admitted mission has no retained dialog map; loading text awaits its provider"));
    }
    const std::string prepare = lookup(input.battle == LoadingBattle::space ? "TEXT_PREPARE_FOR_SPACE_BATTLE"
                                                                              : "TEXT_PREPARE_FOR_LAND_BATTLE");
    if (input.exists) {
        if (const std::string icon = loading_map_icon_path(input.map_path); !icon.empty() && input.exists(icon)) {
            out.map_icon = icon;
        }
    }

    const std::string context = input.context_name ? trimmed(*input.context_name) : std::string();
    const data::Definition* planet = input.objects != nullptr && !context.empty()
        ? input.objects->find(context, data::Category::game_object) : nullptr;
    if (planet == nullptr || !equal_name(planet->type_name, "Planet")) {
        // BLD-09: no planet the data describe. The screen keeps its title and the map's own preview.
        out.diagnostics.push_back(warning(diagnostic_codes::loading_planet,
            context.empty() ? "the map names no planet (root field 0x09); the loading screen describes no planet"
                            : "no Planet object is named " + context + "; the loading screen describes no planet"));
        const std::string name = !input.map_name.empty() ? input.map_name : context;
        out.title = mission ? name : name.empty() ? prepare : prepare + " " + name;
        return out;
    }
    auto resolved = input.objects->resolve(context, data::Category::game_object);
    if (!resolved) {
        out.diagnostics.push_back(warning(diagnostic_codes::loading_planet,
            "Planet " + context + " does not resolve; the loading screen describes no planet"));
        out.title = mission ? context : prepare + " " + context;
        return out;
    }
    out.planet = planet->id;
    const auto tag = [&](const std::string_view name) {
        const data::EffectiveValue* value = resolved.value().value(name);
        return value == nullptr ? std::string() : trimmed(value->value.raw_text);
    };
    // BLD-02: the title is the "battle at" text and the planet's display name (its Text_ID).
    const std::string name = lookup(tag("Text_ID"));
    const auto display_name = name.empty() ? context : name;
    // The campaign dialog formats the same two texts in the same order (WGP-31, BLD-02).
    out.title = mission ? display_name : prepare + " " + display_name;
    // BLD-03: the texts are the planet's Describe_* keys through the text database.
    out.history = lookup(tag("Describe_History"));
    if (input.campaign) {
        out.tactical = lookup(tag("Describe_Tactical"));
        out.advantage = lookup(tag("Describe_Advantage"));
    }
    out.weather = lookup(tag("Describe_Weather"));
    out.terrain = lookup(tag("Describe_Terrain"));
    out.population = lookup(tag("Describe_Population"));
    // BLD-04: the pictures are the planet's own.
    out.native_icon = tag("Native_Icon_Name");
    if (const std::string weather_icon = tag("Encyclopedia_Weather_Icon"); !weather_icon.empty()) {
        out.weather_icon = loading_weather_icon(weather_icon);
    }
    return out;
}

void LoadProgress::report(const std::size_t stage, const std::uint64_t done, const std::uint64_t total) noexcept {
    ++reports_;
    if (finished_ || stage >= load_stage_count) return;
    double before = 0.0;
    for (std::size_t index = 0; index < stage; ++index) before += battle_load_stages[index].weight;
    const double inner = total == 0 ? 1.0 : static_cast<double>(std::min(done, total)) / static_cast<double>(total);
    // One is reserved for complete(): a bar that reads full is a bar that has finished.
    const double value = std::min(before + battle_load_stages[stage].weight * inner, 0.999);
    if (value > fraction_) fraction_ = value;
    if (stage > stage_) stage_ = stage;
}

void LoadProgress::complete() noexcept {
    ++reports_;
    stage_ = load_stage_count - 1U;
    finished_ = true;
}

std::string_view LoadProgress::stage_name() const noexcept { return battle_load_stages[stage_].name; }

} // namespace eawr::presentation::ui
