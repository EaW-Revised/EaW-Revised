#include "live_session_view.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/units/unit_tables.hpp"

#include "viewer_path.hpp"

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace eawr::presentation::godot_backend {
namespace {

namespace tactical = sim::tactical;

// GAMECONSTANTS.XML Shield_Flash_Scale and Shield_Flash_Duration (seconds), BP-21.
constexpr std::array<double, 3> shield_flash_scale{1.0, 1.1, 1.25};
constexpr double shield_flash_duration = 0.1;

[[nodiscard]] std::string json(const std::string_view text) {
    std::string result{"\""};
    for (const char character : text) {
        if (character == '"' || character == '\\') {
            result += '\\';
            result += character;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            result += ' ';
        } else {
            result += character;
        }
    }
    return result + "\"";
}

[[nodiscard]] std::optional<std::uint64_t> parse_u64(const std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty()) return std::nullopt;
    return value;
}

[[nodiscard]] std::optional<double> parse_double(const std::string& text) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}

[[nodiscard]] std::string lower_path(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        character = character == '\\' ? '/' : static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return result;
}

// How far a driven frame's presentation tick may sit from a capture tick it shows.
constexpr double capture_tolerance = 1.0e-6;
// #497: how many shooter and target pairs the report keeps the first hit of.
constexpr std::size_t first_hits_limit = 4096;
// #535: how many rows the report's fading_log keeps (one row a fading entity a reached tick).
constexpr std::size_t fading_log_limit = 8192;

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

// The mounted FoC view's unit tables (the M2 types), or nothing with `failure` set.
[[nodiscard]] std::optional<units::UnitTables> load_tables(const vfs::Vfs& filesystem, const data::Catalog& catalog,
                                                           std::string& failure,
                                                           const std::set<std::string>& additional_types = {}) {
    scene::VfsAssetCache cache(filesystem);
    units::LoadInput input;
    input.catalog = &catalog;
    input.filesystem = &filesystem;
    input.model = cache.access().model;
    if (!additional_types.empty()) {
        for (const auto type : units::pinned_m2_types()) input.types.emplace_back(type);
        input.types.insert(input.types.end(), additional_types.begin(), additional_types.end());
        for (const auto type : units::pinned_m2_obstacles()) input.obstacles.emplace_back(type);
    }
    auto tables = units::load_unit_tables(input);
    if (!tables) {
        failure = "live session unit tables: " + core::format_diagnostic(tables.error());
        return std::nullopt;
    }
    return std::move(tables).value();
}

// <tick>:<move|face|stop>:<unit>[@<x>,<y>,<z>], <tick>:damage:<unit>@<amount>[,<hardpoint>],
// <tick>:attack:<unit>@<target unit>, <tick>:<attack_move|guard>:<unit>@<x>,<y>,<z> or
// @<target unit> (#452: a point, or the unit to approach or guard), or
// <tick>:ability:<unit>@<ABILITY>[,on|off|autofire|manual[,<target unit>]] (#76: switch a unit's
// ability, space-abilities AB-10; #561: a targeted one, ION_CANNON_SHOT, switches on at a target)
// (scripted damage, HD-30, to the hull or to the hardpoint of that HardPoints index; #81 captures
// destroy a unit with it, #391 captures a hardpoint)
[[nodiscard]] std::optional<LiveSessionView::ScheduledOrder> parse_order(const std::string& text) {
    const std::size_t first = text.find(':');
    const std::size_t second = first == std::string::npos ? std::string::npos : text.find(':', first + 1);
    if (second == std::string::npos) return std::nullopt;
    LiveSessionView::ScheduledOrder order;
    const auto tick = parse_u64(std::string_view(text).substr(0, first));
    const std::string kind = text.substr(first + 1, second - first - 1);
    const std::size_t at = text.find('@', second + 1);
    // #552: `2+3+4` names several units of one command, as a selection's order does.
    const std::string units = text.substr(second + 1, at == std::string::npos ? std::string::npos : at - second - 1);
    std::optional<std::uint64_t> unit;
    for (std::size_t start = 0;;) {
        const std::size_t plus = units.find('+', start);
        const auto one = parse_u64(std::string_view(units).substr(start, plus == std::string::npos ? std::string::npos : plus - start));
        if (!one || *one == 0) return std::nullopt;
        if (!unit) unit = one;
        else order.more.push_back(*one);
        if (plus == std::string::npos) break;
        start = plus + 1;
    }
    if (!tick || !unit) return std::nullopt;
    order.tick = *tick;
    order.unit = *unit;
    if (kind == "move") order.kind = tactical::OrderKind::move;
    else if (kind == "face") order.kind = tactical::OrderKind::face;
    else if (kind == "stop") order.kind = tactical::OrderKind::stop;
    else if (kind == "damage") order.kind = tactical::OrderKind::damage;
    else if (kind == "attack") order.kind = tactical::OrderKind::attack;
    else if (kind == "attack_move") order.kind = tactical::OrderKind::attack_move;
    else if (kind == "guard") order.kind = tactical::OrderKind::guard;
    else if (kind == "ability") order.kind = tactical::OrderKind::ability;
    else return std::nullopt;
    if ((order.kind == tactical::OrderKind::stop) != (at == std::string::npos)) return std::nullopt;
    if (order.kind == tactical::OrderKind::ability) {
        const std::string rest = text.substr(at + 1);
        const std::size_t comma = rest.find(',');
        order.ability = tactical::ability_kind(rest.substr(0, comma));
        if (order.ability == tactical::AbilityKind::none) return std::nullopt;
        std::string action = comma == std::string::npos ? std::string("on") : rest.substr(comma + 1);
        if (const std::size_t aim = action.find(','); aim != std::string::npos) {
            const auto target = parse_u64(std::string_view(action).substr(aim + 1));
            if (!target || *target == 0) return std::nullopt;
            order.target = *target;
            action.resize(aim);
        }
        if (action == "on") order.action = tactical::AbilityAction::activate;
        else if (action == "off") order.action = tactical::AbilityAction::deactivate;
        else if (action == "autofire") order.action = tactical::AbilityAction::autofire_on;
        else if (action == "manual") order.action = tactical::AbilityAction::autofire_off;
        else return std::nullopt;
        return order;
    }
    const bool names_unit = order.kind == tactical::OrderKind::attack
        || ((order.kind == tactical::OrderKind::attack_move || order.kind == tactical::OrderKind::guard)
            && text.find(',', at + 1) == std::string::npos);
    if (names_unit) {
        const auto target = parse_u64(std::string_view(text).substr(at + 1));
        if (!target || *target == 0) return std::nullopt;
        order.target = *target;
        return order;
    }
    if (at != std::string::npos) {
        std::vector<sim::math::Fixed> values;
        std::size_t start = at + 1;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            const auto value = parse_double(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!value) return std::nullopt;
            auto fixed = scene::fixed_from_binary32(static_cast<float>(*value));
            if (!fixed) return std::nullopt;
            values.push_back(fixed.value());
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (order.kind == tactical::OrderKind::damage) {
            if (values.empty() || values.size() > 2 || values[0].raw() < 0) return std::nullopt;
            order.amount = values[0];
            if (values.size() == 2) {
                const std::size_t comma = text.find(',', at + 1);
                const auto hardpoint = parse_u64(std::string_view(text).substr(comma + 1));
                if (!hardpoint || *hardpoint >= 0xffffffffU) return std::nullopt;
                order.hardpoint = static_cast<std::uint32_t>(*hardpoint);
            }
            return order;
        }
        if (values.size() != 3) return std::nullopt;
        order.point = {values[0], values[1], values[2]};
    }
    return order;
}

[[nodiscard]] bool iequal(const std::string_view left, const std::string_view right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] std::string tag_text(const data::EffectiveObject& object, const std::string_view tag) {
    const data::EffectiveValue* value = object.value(tag);
    if (value == nullptr) return {};
    std::string_view text = value->value.raw_text;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) text.remove_suffix(1);
    return std::string(text);
}

// The space model an object type draws, as a logical path; empty when it names none.
[[nodiscard]] std::string space_model_path(const data::EffectiveObject& object) {
    std::string name = tag_text(object, "Space_Model_Name");
    if (name.empty()) name = tag_text(object, "Model_Name");
    return name.empty() ? std::string{} : lower_path("data/art/models/" + name);
}

// A Death_* seconds tag of the clone type; `fallback` when absent or not a number.
[[nodiscard]] double seconds_tag(const data::EffectiveObject& object, const std::string_view tag, const double fallback) {
    std::string text = tag_text(object, tag);
    if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) text.pop_back();
    return parse_double(text).value_or(fallback);
}

// The object type a destroyed unit of this type becomes (its Death_Clone entries, UA-P1).
[[nodiscard]] std::optional<std::string> death_clone_of(const data::EffectiveObject& object) {
    std::vector<std::string> entries;
    for (const data::EffectiveValue& value : object.values) {
        if (iequal(value.value.name, "Death_Clone")) entries.push_back(value.value.raw_text);
    }
    return animation::death_clone_type(entries);
}

// The clip types #81 selects from tactical state, for the report.
constexpr std::array<std::size_t, 8> reported_clip_types{
    animation::clip_types::idle, animation::clip_types::space_idle, animation::clip_types::move,
    animation::clip_types::attack, animation::clip_types::attack_idle, animation::clip_types::die,
    animation::clip_types::deploy, animation::clip_types::undeploy};
[[nodiscard]] std::optional<std::array<double, 3>> parse_point(const std::string& text) {
    if (text.empty() || text.front() != '@') return std::nullopt;
    std::array<double, 3> point{};
    std::size_t start = 1;
    for (std::size_t axis = 0; axis < 3; ++axis) {
        const std::size_t comma = text.find(',', start);
        if ((axis < 2) == (comma == std::string::npos)) return std::nullopt;
        const auto value = parse_double(text.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!value) return std::nullopt;
        point[axis] = *value;
        start = comma + 1;
    }
    return point;
}

// screen=x,y: a viewport pixel, kept as (x, y, 0).
[[nodiscard]] std::optional<std::array<double, 3>> parse_screen(const std::string& text) {
    if (text.rfind("screen=", 0) != 0) return std::nullopt;
    const std::size_t comma = text.find(',', 7);
    if (comma == std::string::npos) return std::nullopt;
    const auto x = parse_double(text.substr(7, comma - 7));
    const auto y = parse_double(text.substr(comma + 1));
    if (!x || !y) return std::nullopt;
    return std::array<double, 3>{*x, *y, 0.0};
}

// #455: minimap=x,y, a minimap point with x and y from -1 to 1 (+y up).
[[nodiscard]] std::optional<std::array<double, 3>> parse_minimap(const std::string& text) {
    if (text.rfind("minimap=", 0) != 0) return std::nullopt;
    const std::size_t comma = text.find(',', 8);
    if (comma == std::string::npos) return std::nullopt;
    const auto x = parse_double(text.substr(8, comma - 8));
    const auto y = parse_double(text.substr(comma + 1));
    if (!x || !y || std::abs(*x) > 1.0 || std::abs(*y) > 1.0) return std::nullopt;
    return std::array<double, 3>{*x, *y, 0.0};
}

// <tick>:<kind>:<target>[+shift][+ctrl][+alt] (see ScriptedInput).
[[nodiscard]] std::optional<LiveSessionView::ScriptedInput> parse_input(std::string text) {
    LiveSessionView::ScriptedInput input;
    for (;;) {
        const std::size_t plus = text.rfind('+');
        if (plus == std::string::npos || plus + 1 == text.size() || plus == 0) break;
        const std::string modifier = text.substr(plus + 1);
        if (modifier == "shift") input.shift = true;
        else if (modifier == "ctrl") input.ctrl = true;
        else if (modifier == "alt") input.alt = true;
        else break;
        text.resize(plus);
    }
    const std::size_t first = text.find(':');
    const std::size_t second = first == std::string::npos ? std::string::npos : text.find(':', first + 1);
    if (second == std::string::npos) return std::nullopt;
    if (text[0] == 'f') {
        // f<frame>: the shown frame after the warm-up (#459: a paused battle's tick holds).
        const auto frame = parse_u64(std::string_view(text).substr(1, first - 1));
        if (!frame) return std::nullopt;
        input.frame = *frame;
    } else {
        const auto tick = parse_u64(std::string_view(text).substr(0, first));
        if (!tick) return std::nullopt;
        input.tick = *tick;
    }
    input.kind = text.substr(first + 1, second - first - 1);
    const std::string target = text.substr(second + 1);
    if (input.kind == "key") {
        if (target.empty()) return std::nullopt;
        input.key = target;
        return input;
    }
    if (input.kind == "wheel") {
        if (target != "out" || input.shift || input.ctrl || input.alt) return std::nullopt;
        return input;
    }
    if (input.kind == "box") {
        const std::size_t slash = target.find('/');
        if (slash == std::string::npos) return std::nullopt;
        input.screen = target.rfind("screen=", 0) == 0;
        input.minimap = target.rfind("minimap=", 0) == 0;
        const auto parse = input.minimap ? parse_minimap : input.screen ? parse_screen : parse_point;
        const auto from = parse(target.substr(0, slash));
        const auto to = parse(target.substr(slash + 1));
        if (!from || !to) return std::nullopt;
        input.points = {*from, *to};
        return input;
    }
    if (input.kind == "mclick") {
        if (target != "centre" || input.shift || input.alt) return std::nullopt;
        return input;
    }
    if (input.kind == "mdrag") {
        const std::size_t comma = target.find(',');
        if (comma == std::string::npos || input.shift || input.alt) return std::nullopt;
        const auto dx = parse_double(target.substr(0, comma));
        const auto dy = parse_double(target.substr(comma + 1));
        if (!dx || !dy || std::abs(*dx) > 4096.0 || std::abs(*dy) > 4096.0) return std::nullopt;
        input.drag = {*dx, *dy};
        return input;
    }
    // #459 and #453: a HUD control by name. #530 adds the production panel's controls: the
    // reinforcements button, the pane's close button and slots r_RRCC, and the queue slots tqueueNN.
    if (target.rfind("hud=", 0) == 0) {
        input.hud = target.substr(4);
        if (input.kind != "click" && input.kind != "hover" && input.kind != "press" && input.kind != "release") return std::nullopt;
        const auto digits = [](std::string_view text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        const std::string_view hud = input.hud;
        const bool time = hud == "pause" || hud == "fast_forward" || hud == "resume" || hud == "quit";
        const bool production = hud == "b_reinforcement" || hud == "r_close" ||
                                (hud.size() == 6 && hud.substr(0, 2) == "r_" && digits(hud.substr(2))) ||
                                (hud.size() == 8 && hud.substr(0, 6) == "tqueue" && digits(hud.substr(6)));
        if (!time && !production) return std::nullopt;
        if (input.shift || input.ctrl || input.alt) return std::nullopt;
        return input;
    }
    // #454: an ability button of the HUD's command bar, by its index among the shown buttons.
    if (target.rfind("ability=", 0) == 0) {
        if (input.kind != "click" && input.kind != "rclick" && input.kind != "hover") return std::nullopt;
        const auto index = parse_u64(std::string_view(target).substr(8));
        if (!index || *index >= 48 || input.shift || input.ctrl || input.alt) return std::nullopt;
        input.ability = static_cast<std::size_t>(*index);
        return input;
    }
    // #425: a unit card of the HUD's command bar, by slot (s_select_<N>); `hover` only moves there.
    if (target.rfind("card=", 0) == 0) {
        if (input.kind != "click" && input.kind != "dclick" && input.kind != "hover") return std::nullopt;
        const auto slot = parse_u64(std::string_view(target).substr(5));
        if (!slot || *slot >= 48 || input.ctrl || input.alt) return std::nullopt;
        input.card = static_cast<std::size_t>(*slot);
        return input;
    }
    if (input.kind != "click" && input.kind != "dclick" && input.kind != "rclick" && input.kind != "hover"
        && input.kind != "press" && input.kind != "release") {
        return std::nullopt;
    }
    if (target.rfind("icon=", 0) == 0 && (input.kind == "hover" || input.kind == "click" || input.kind == "dclick" || input.kind == "rclick")) {
        const auto unit = parse_u64(std::string_view(target).substr(5));
        if (!unit || *unit == 0) return std::nullopt;
        input.unit = *unit;
        input.icon = true;
        return input;
    }
    if (target.rfind("reticle=", 0) == 0) {
        // reticle=<unit>:<hardpoint>
        const auto colon = target.find(':', 8);
        if (colon == std::string::npos) return std::nullopt;
        const auto unit = parse_u64(std::string_view(target).substr(8, colon - 8));
        const std::string_view which = std::string_view(target).substr(colon + 1);
        const auto hardpoint = which == "first" ? std::optional<std::uint64_t>(0xffffffffU) : parse_u64(which);
        if (!unit || *unit == 0 || !hardpoint || (*hardpoint >= 255 && *hardpoint != 0xffffffffU)) return std::nullopt;
        input.unit = *unit;
        input.reticle = static_cast<std::uint32_t>(*hardpoint);
        return input;
    }
    if (target.rfind("unit=", 0) == 0) {
        const std::string_view body = std::string_view(target).substr(5);
        const std::size_t plus = body.find('+');
        const auto unit = parse_u64(body.substr(0, plus));
        if (!unit || *unit == 0) return std::nullopt;
        input.unit = *unit;
        if (plus != std::string_view::npos) {
            // #665: unit=N+@dx,dy,dz aims at a source-space offset from the unit's position.
            const auto offset = parse_point(std::string(body.substr(plus + 1)));
            if (!offset) return std::nullopt;
            input.points = {*offset};
            input.offset = true;
        }
        return input;
    }
    input.screen = target.rfind("screen=", 0) == 0;
    input.minimap = target.rfind("minimap=", 0) == 0;
    const auto point = input.minimap ? parse_minimap(target) : input.screen ? parse_screen(target) : parse_point(target);
    if (!point) return std::nullopt;
    input.points = {*point};
    return input;
}

} // namespace

bool LiveSessionView::parse_argument(const std::string_view name, const std::optional<std::string>& value,
                                     Options& options, bool& value_used, std::string& error) {
    value_used = false;
    if (name.substr(0, 12) != "--eawr-live-" && !name.starts_with("--eawr-skirmish-")) return false;
    if (!value) {
        error = "missing value for " + std::string(name);
        return true;
    }
    value_used = true;
    const std::string& text = *value;
    if (name == "--eawr-live-session") {
        if (text != "m2" && text != "skirmish" && text != "replay" && text != "melee") {
            error = "--eawr-live-session expects m2, skirmish, replay or melee";
        }
        options.fixture = text;
    } else if (name == "--eawr-skirmish-map") {
        options.skirmish.map = lower_path(text);
    } else if (name == "--eawr-skirmish-seed") {
        const auto seed = parse_u64(text);
        if (!seed) error = "--eawr-skirmish-seed expects an unsigned whole number";
        else options.skirmish.seed = *seed;
    } else if (name == "--eawr-skirmish-players") {
        const auto comma = text.find(',');
        const auto first = comma == std::string::npos ? std::nullopt : parse_u64(text.substr(0, comma));
        const auto second = comma == std::string::npos ? std::nullopt : parse_u64(text.substr(comma + 1));
        if (!first || !second || *first == 0 || *first >= *second || *second > tactical::max_players) {
            error = "--eawr-skirmish-players requires exactly two increasing slot IDs; more than two players are not supported";
        } else {
            if (!options.skirmish.slots) options.skirmish.slots = skirmish::m2_fixture().slots;
            (*options.skirmish.slots)[0].slot = static_cast<std::uint32_t>(*first);
            (*options.skirmish.slots)[1].slot = static_cast<std::uint32_t>(*second);
        }
    } else if (name == "--eawr-skirmish-slot" || name == "--eawr-skirmish-fleet") {
        const auto colon = text.find(':');
        const auto slot = parse_u64(text.substr(0, colon));
        if (colon == std::string::npos || !slot || *slot < 1 || *slot > tactical::max_players) {
            error = std::string(name) + " expects a player slot followed by a colon";
        } else {
            if (!options.skirmish.slots) options.skirmish.slots = skirmish::m2_fixture().slots;
            const auto found = std::find_if(options.skirmish.slots->begin(), options.skirmish.slots->end(),
                [&](const auto& entry) { return entry.slot == *slot; });
            if (found == options.skirmish.slots->end()) {
                error = "slot is not one of the two skirmish players; set --eawr-skirmish-players first";
                return true;
            }
            auto& entry = *found;
            const auto body = text.substr(colon + 1);
            if (name == "--eawr-skirmish-fleet") {
                entry.fleet.clear();
                if (body != "none") {
                    std::istringstream names(body);
                    std::string unit;
                    while (std::getline(names, unit, ',')) {
                        if (unit.empty()) error = "--eawr-skirmish-fleet has an empty unit name";
                        else entry.fleet.push_back(unit);
                    }
                    if (entry.fleet.empty() || body.ends_with(',')) error = "--eawr-skirmish-fleet expects unit names or none";
                }
            } else {
                const auto first = body.find(':');
                const auto last = body.rfind(':');
                const auto team = first == last ? std::nullopt : parse_u64(body.substr(first + 1, last - first - 1));
                const auto control = last == std::string::npos ? std::string{} : body.substr(last + 1);
                if (first == std::string::npos || first == 0 || !team || *team >= tactical::max_players
                    || (control != "human" && control != "ai")) {
                    error = "--eawr-skirmish-slot expects <1|2>:<faction>:<team>:<human|ai>";
                } else {
                    entry.faction = body.substr(0, first);
                    entry.team = static_cast<std::uint32_t>(*team);
                    entry.human = control == "human";
                }
            }
        }
    } else if (name == "--eawr-live-replay") {
        options.replay_input = ViewerPath{text}.native();
    } else if (name == "--eawr-live-melee") {
        options.melee_size = skirmish::melee_size(text);
        if (!options.melee_size) error = "--eawr-live-melee expects s, m or l";
    } else if (name == "--eawr-live-melee-seed") {
        std::uint64_t seed{};
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), seed);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) {
            error = "--eawr-live-melee-seed expects a whole number";
        }
        options.melee_seed = seed;
    } else if (name == "--eawr-live-ai") {
        if (text != "on" && text != "off") error = "--eawr-live-ai expects on or off";
        options.ai = text == "on";
    } else if (name == "--eawr-live-reveal") {
        if (text != "on" && text != "off") error = "--eawr-live-reveal expects on or off";
        options.reveal = text == "on";
    } else if (name == "--eawr-live-deploy-overlay") {
        if (text != "on" && text != "off") error = "--eawr-live-deploy-overlay expects on or off";
        options.deploy_overlay = text == "on";
    } else if (name == "--eawr-live-ability-demo") {
        if (text != "on" && text != "off") error = "--eawr-live-ability-demo expects on or off";
        options.ability_demo = text == "on";
    } else if (name == "--eawr-live-defend") {
        if (text != "on" && text != "off") error = "--eawr-live-defend expects on or off";
        options.defend = text == "on";
    } else if (name == "--eawr-live-purchase-slots") {
        const auto slots = parse_u64(text);
        if (!slots || *slots == 0 || *slots > 64) error = "--eawr-live-purchase-slots expects 1 to 64";
        else options.purchase_slots = static_cast<std::uint32_t>(*slots);
    } else if (name == "--eawr-live-late-orders") {
        if (text != "on" && text != "off") error = "--eawr-live-late-orders expects on or off";
        options.late_orders = text == "on";
    } else if (name == "--eawr-live-shield-flash") {
        if (text != "on" && text != "off") error = "--eawr-live-shield-flash expects on or off";
        options.shield_flash = text == "on";
    } else if (name == "--eawr-live-player") {
        const auto player = parse_u64(text);
        if (!player || *player == 0 || *player > tactical::max_players) error = "--eawr-live-player expects a player ID";
        else options.player = static_cast<tactical::PlayerId>(*player);
    } else if (name == "--eawr-live-order") {
        auto order = parse_order(text);
        if (!order || options.orders.size() >= 256) {
            error = "--eawr-live-order expects <tick>:<move|face>:<unit>@<x>,<y>,<z>, <tick>:stop:<unit>, "
                    "<tick>:damage:<unit>@<amount>[,<hardpoint>], <tick>:attack:<unit>@<target unit> or "
                    "<tick>:ability:<unit>@<ABILITY>[,on|off|autofire|manual[,<target unit>]]";
        } else {
            options.orders.push_back(*order);
        }
    } else if (name == "--eawr-live-input") {
        auto input = parse_input(text);
        if (!input || options.inputs.size() >= 256) {
            error = "--eawr-live-input expects <tick>:<click|dclick|rclick>:<unit=N[+@dx,dy,dz]|@x,y,z|screen=x,y>, "
                    "<tick>:<click|dclick|hover>:card=N, <tick>:<click|dclick|rclick|hover>:icon=N, "
                    "<tick>:<click|rclick|hover>:ability=N, "
                    "<tick>:box:@x,y,z/@x,y,z or screen=x,y/screen=x,y, "
                    "<tick>:<click|rclick>:minimap=x,y or <tick>:box:minimap=x,y/minimap=x,y, "
                    "<tick>:key:<name>, each with optional +shift, +ctrl or +alt, "
                    "<tick>:mdrag:<dx>,<dy> or <tick>:mclick:centre, each with optional +ctrl, "
                    "or <tick>:wheel:out, or <tick>:<click|hover>:hud=<pause|fast_forward|resume|quit|"
                    "b_reinforcement|r_close|r_RRCC|tqueueNN>; "
                    "f<frame> in place of <tick> fires on that frame after the warm-up";
        } else {
            options.inputs.push_back(std::move(*input));
        }
    } else if (name == "--eawr-live-capture-ticks") {
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            const auto tick = parse_u64(std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!tick || options.capture_ticks.size() >= 256) {
                error = "--eawr-live-capture-ticks expects ascending ticks a,b,c";
                break;
            }
            if (!options.capture_ticks.empty() && *tick <= options.capture_ticks.back()) {
                error = "--eawr-live-capture-ticks expects ascending ticks a,b,c";
                break;
            }
            options.capture_ticks.push_back(*tick);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    } else if (name == "--eawr-live-follow-group") {
        const std::string usage = "--eawr-live-follow-group expects <tick>:<entity>[,<entity>...] in tick order";
        const std::size_t colon = text.find(':');
        const auto tick = colon == std::string::npos ? std::nullopt : parse_u64(std::string_view(text).substr(0, colon));
        Options::FollowGroup follow;
        if (tick && (options.follow_groups.empty() || *tick > options.follow_groups.back().tick) && options.follow_groups.size() < 256) {
            follow.tick = *tick;
            std::size_t start = colon + 1;
            for (;;) {
                const std::size_t comma = text.find(',', start);
                const auto entity = parse_u64(std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
                if (!entity || *entity == 0 || follow.entities.size() >= 64) {
                    follow.entities.clear();
                    break;
                }
                follow.entities.push_back(static_cast<sim::EntityId>(*entity));
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        if (follow.entities.empty()) error = usage;
        else options.follow_groups.push_back(std::move(follow));
    } else if (name == "--eawr-live-capture-frames") {
        std::size_t start = 0;
        for (;;) {
            const std::size_t comma = text.find(',', start);
            const auto frame = parse_u64(std::string_view(text).substr(start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!frame || *frame == 0 || options.capture_frames.size() >= 256
                || (!options.capture_frames.empty() && *frame <= options.capture_frames.back())) {
                error = "--eawr-live-capture-frames expects ascending frames a,b,c from 1";
                break;
            }
            options.capture_frames.push_back(*frame);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    } else if (name == "--eawr-live-ticks") {
        const auto tick = parse_u64(text);
        if (!tick || *tick > tactical::max_ticks) error = "--eawr-live-ticks expects a tick count";
        else options.end_tick = *tick;
    } else if (name == "--eawr-live-step") {
        const auto step = parse_double(text);
        if (!step || *step <= 0.0 || *step > 30.0) error = "--eawr-live-step expects ticks per frame in (0, 30]";
        else options.ticks_per_frame = *step;
    } else if (name == "--eawr-live-speed") {
        const auto step = parse_u64(text);
        if (!step || *step >= ui::speed_step_rates.size()) error = "--eawr-live-speed expects a speed step 0 to 4";
        else options.speed_step = static_cast<std::uint32_t>(*step);
    } else if (name == "--eawr-live-workers") {
        const auto workers = parse_u64(text);
        if (!workers || *workers == 0 || *workers > 256) error = "--eawr-live-workers expects 1 to 256";
        else options.workers = static_cast<std::size_t>(*workers);
    } else if (name == "--eawr-live-particle-workers") {
        const auto workers = parse_u64(text);
        if (!workers || *workers == 0 || *workers > 64) error = "--eawr-live-particle-workers expects 1 to 64";
        else options.particle_workers = static_cast<std::size_t>(*workers);
    } else if (name == "--eawr-live-fault-tick") {
        const auto tick = parse_u64(text);
        if (!tick || *tick > tactical::max_ticks) error = "--eawr-live-fault-tick expects a tick";
        else options.fault_tick = *tick;
    } else if (name == "--eawr-live-stall") {
        const std::size_t colon = text.find(':');
        const bool start = colon != std::string::npos && std::string_view(text).substr(0, colon) == "start";
        const auto tick = colon == std::string::npos ? std::nullopt
            : start ? std::optional<std::uint64_t>{0} : parse_u64(std::string_view(text).substr(0, colon));
        const auto ticks = colon == std::string::npos ? std::nullopt : parse_u64(std::string_view(text).substr(colon + 1));
        if (!tick || !ticks || *ticks == 0 || *tick > tactical::max_ticks || *ticks > tactical::max_ticks) {
            error = "--eawr-live-stall expects <tick|start>:<ticks skipped>";
        } else {
            options.stall = Options::Stall{*tick, *ticks, start};
        }
    } else if (name == "--eawr-live-follow") {
        const auto unit = parse_u64(text);
        if (!unit || *unit == 0) error = "--eawr-live-follow expects a unit ID";
        else options.follow = static_cast<sim::EntityId>(*unit);
    } else if (name == "--eawr-live-death-anim") {
        if (!animation::clip_type_index(text)) error = "--eawr-live-death-anim expects a clip type such as DIE";
        else options.death_anim_type = text;
    } else if (name == "--eawr-live-death-persistence") {
        const auto seconds = parse_double(text);
        if (!seconds || !animation::seconds_to_ticks(*seconds, tactical::logical_frames_per_second)) {
            error = "--eawr-live-death-persistence expects seconds, 0 or more";
        } else {
            options.death_persistence = *seconds;
        }
    } else if (name == "--eawr-live-audio-pace") {
        if (text != "on" && text != "off") error = "--eawr-live-audio-pace expects on or off";
        options.audio_pace = text == "on";
    } else if (name == "--eawr-live-verify") {
        if (text != "on" && text != "off") error = "--eawr-live-verify expects on or off";
        options.verify = text == "on";
    } else if (name == "--eawr-live-hashes") {
        options.hashes_path = ViewerPath{text}.native();
    } else if (name == "--eawr-live-replay-out") {
        options.replay_path = ViewerPath{text}.native();
    } else {
        value_used = false;
        error = "unknown live session option " + std::string(name);
    }
    return true;
}

LiveSessionView::LiveSessionView(Options options) : options_(std::move(options)), time_(options_.speed_step) {}

void LiveSessionView::apply_time() {
    if (!session_) return;
    session_->set_paused(!time_.running());
    session_->set_target_rate(time_.target_rate());
}

void LiveSessionView::press_pause() {
    if (session_ && time_.press_pause(session_->completed_tick())) apply_time();
}

void LiveSessionView::press_fast_forward() {
    if (session_ && time_.press_fast_forward(session_->completed_tick())) apply_time();
}

void LiveSessionView::resume() {
    if (session_ && time_.resume(session_->completed_tick())) apply_time();
}

LiveSessionView::~LiveSessionView() {
    shutdown_trace::mark("live session view: stop begins");
    if (session_) session_->stop();
    shutdown_trace::mark("live session view: stopped");
}

bool LiveSessionView::prepare_m2(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure) {
    skirmish::Fixture fixture = skirmish::m2_fixture();
    if (options_.fixture == "skirmish") {
        auto selected = skirmish::fixture_from_options(options_.skirmish, filesystem, catalog);
        if (!selected) {
            failure = "live skirmish options: " + core::format_diagnostic(selected.error());
            return false;
        }
        fixture = std::move(selected).value();
    }
    auto tables = load_tables(filesystem, catalog, failure);
    if (!tables) return false;
    auto inputs = skirmish::read_start_inputs(fixture, filesystem, catalog, *tables);
    if (!inputs) {
        failure = "live session start inputs: " + core::format_diagnostic(inputs.error());
        return false;
    }
    if (options_.fixture == "skirmish") {
        // SC-01: the existing loader follows the selected types' craft, hangar,
        // hardpoint and projectile references. Keep its pinned inputs unchanged
        // when every selected type is already loaded (including the M2 default).
        std::set<std::string> missing;
        const auto need = [&](const std::string& type) { if (!tables->find(type)) missing.insert(type); };
        for (const auto& slot : fixture.slots) {
            for (const auto& type : slot.fleet) need(type);
            for (const auto& forces : inputs.value().faction_forces) {
                if (lower_path(forces.faction) != lower_path(slot.faction)) continue;
                for (const auto& type : forces.space_skirmish_default_forces) need(type);
            }
            const std::string station = "team_" + std::string(slot.team < 10 ? "0" : "")
                + std::to_string(slot.team) + "_space_station";
            for (const auto& placement : inputs.value().placements) {
                if (!placement.marker || lower_path(placement.type) != station) continue;
                for (const auto& candidate : placement.marker_for) {
                    std::string affiliations = candidate.affiliation;
                    std::replace(affiliations.begin(), affiliations.end(), ',', ' ');
                    std::istringstream names(affiliations);
                    std::string faction;
                    while (names >> faction) if (lower_path(faction) == lower_path(slot.faction)) need(candidate.type);
                }
            }
        }
        if (!missing.empty()) {
            auto extended = load_tables(filesystem, catalog, failure, missing);
            if (!extended) return false;
            tables = std::move(extended);
            inputs.value().tables = &*tables;
        }
    }
    auto start = skirmish::build_start(fixture, inputs.value());
    if (!start) {
        failure = "live session start: " + core::format_diagnostic(start.error());
        return false;
    }
    // #76: the ability table's human players are the start's human lobby players, as the victory rules'.
    std::vector<tactical::PlayerId> humans;
    for (const skirmish::StartPlayer& player : start.value().players) {
        if (player.lobby && player.human) humans.push_back(player.player.player_id);
    }
    auto content = skirmish::session_content(*tables, humans);
    if (!content) {
        failure = "live session content: " + core::format_diagnostic(content.error());
        return false;
    }
    // #495: the map's fog grid. #507's live `--eawr-live-reveal` is a draw-time bypass only
    // (interpolate_units, LiveFogView, the minimap) and must never change what the session itself
    // binds: unlike a revealed replay (below), a live m2 session keeps its real sensor table on
    // reveal, so skipping fog here would silently swap visible_to's fog-cell test (V-11 to V-17)
    // for the exact-range one, changing hashes reveal on vs off.
    {
        auto fog = skirmish::fog_rules(inputs.value());
        if (!fog) {
            failure = "live session fog rules: " + core::format_diagnostic(fog.error());
            return false;
        }
        content.value().fog = fog.value();
    }
    if (options_.ai.value_or(true)) {
        // #79: the retail freestore of each AI player, from the mounted FoC scripts.
        auto ai = platform::live_ai(start.value(), inputs.value(), *tables, filesystem);
        if (!ai) {
            failure = "live session AI scripts: " + core::format_diagnostic(ai.error());
            return false;
        }
        ai_scripts_ = std::move(ai.value().scripts);
        ai_players_ = std::move(ai.value().players);
    }
    start_ = std::move(start).value();
    setup_ = start_->setup;
    content_ = std::move(content).value();
    victory_ = skirmish::victory_rules(*start_, tables.value());
    // #530: the skirmish economy (credits, the station build queues and hyperspace arrival).
    auto economy = skirmish::economy_rules(*start_, inputs.value(), tables.value());
    if (!economy) {
        failure = "live session economy: " + core::format_diagnostic(economy.error());
        return false;
    }
    economy_ = std::move(economy).value();
    auto preview_colours = ui::reinforcement_colours(filesystem);
    if (!preview_colours) {
        failure = "reinforcement preview colours: " + core::format_diagnostic(preview_colours.error());
        return false;
    }
    preview_colours_ = preview_colours.value();
    tables_ = std::move(*tables);

    const auto human = std::find_if(start_->players.begin(), start_->players.end(),
        [](const skirmish::StartPlayer& player) { return player.human; });
    player_ = options_.player.value_or(human != start_->players.end() ? human->player.player_id : 1U);
    if (std::none_of(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == player_; })) {
        failure = "--eawr-live-player " + std::to_string(player_) + " is not a player of the start";
        return false;
    }
    for (const skirmish::StartPlayer& player : start_->players) {
        team_of_player_[player.player.player_id] = player.player.team_id;
    }
    for (const skirmish::StartUnit& unit : start_->units) {
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = unit.type;
        ship.position = {to_float(unit.state.position.x), to_float(unit.state.position.y), to_float(unit.state.position.z)};
        ship.yaw_degrees = to_float(unit.yaw_degrees);
        ship.live_entity = unit.state.entity_id;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == unit.state.owner; });
        if (owner != start_->players.end() && owner->colour) ship.team_colour = owner->colour->rgb;
        if (unit.role == skirmish::UnitRole::map_object) {
            // The map draws this record's object; the session owns it now. Its team colour
            // stays the one the map's owner gives it.
            session_records_.push_back(unit.record);
            ship.colour_record = unit.record;
        }
        ship_of_entity_.emplace(unit.state.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.state.entity_id, unit.state.owner);
        placed_ships_.push_back(std::move(ship));
    }
    return true;
}

std::optional<std::array<std::uint8_t, 3>> LiveSessionView::player_colour(const tactical::PlayerId player) const {
    if (!start_) return std::nullopt;
    for (const skirmish::StartPlayer& entry : start_->players) {
        if (entry.player.player_id == player && entry.colour) return entry.colour->rgb;
    }
    return std::nullopt;
}

bool LiveSessionView::squadron_launched(const sim::EntityId container) const {
    // A squadron the setup does not list came out of a hangar (#518); with no setup to compare
    // with, none is known to have (the least visible answer).
    return setup_ && squadrons_.contains(container)
        && std::none_of(setup_->squadrons.begin(), setup_->squadrons.end(),
                        [&](const tactical::Squadron& start) { return start.container == container; });
}

bool LiveSessionView::is_ally_of_local(const tactical::PlayerId owner) const {
    if (owner == player_) return true;
    const auto own = team_of_player_.find(player_);
    const auto other = team_of_player_.find(owner);
    return own != team_of_player_.end() && other != team_of_player_.end() && own->second == other->second;
}

bool LiveSessionView::prepare_replay(const vfs::Vfs& filesystem, const data::Catalog& catalog, std::string& failure) {
    auto tables = load_tables(filesystem, catalog, failure);
    if (!tables) return false;
    auto replay = [&]() -> std::optional<tactical::TacticalReplay> {
        if (options_.fixture == "melee") {
            // #601: the melee benchmark's fight, built here exactly as path_bench --melee builds it.
            auto inputs = skirmish::read_start_inputs(skirmish::m2_fixture(), filesystem, catalog, *tables);
            if (!inputs) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(inputs.error());
                return std::nullopt;
            }
            auto start = skirmish::build_start(skirmish::m2_fixture(), inputs.value());
            if (!start) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(start.error());
                return std::nullopt;
            }
            auto melee = skirmish::build_melee(*tables, start.value(), options_.melee_size.value_or(skirmish::MeleeSize::s),
                                               options_.melee_seed.value_or(601), 4500);
            if (!melee) {
                failure = "--eawr-live-session melee: " + core::format_diagnostic(melee.error());
                return std::nullopt;
            }
            return std::move(melee).value().replay;
        }
        std::ifstream file(options_.replay_input, std::ios::binary);
        if (!file) {
            failure = "--eawr-live-replay: cannot read " + ViewerPath::utf8(options_.replay_input);
            return std::nullopt;
        }
        const std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto parsed = tactical::parse_replay(bytes, ViewerPath::utf8(options_.replay_input));
        if (!parsed) {
            failure = "--eawr-live-replay: " + core::format_diagnostic(parsed.error());
            return std::nullopt;
        }
        return std::move(parsed).value();
    }();
    if (!replay) return false;
    if (units::content_identity(*tables) != replay.value().setup.content_identity) {
        failure = "--eawr-live-replay: the replay names other content than the mounted FoC unit tables";
        return false;
    }
    // #77, #76: like sim_headless, the replay's human players are the pinned fixture's human slots.
    auto content = skirmish::session_content(*tables, skirmish::human_slots(skirmish::m2_fixture()));
    if (!content) {
        failure = "live session content: " + core::format_diagnostic(content.error());
        return false;
    }
    const tactical::TacticalSetup& setup = replay.value().setup;
    if (options_.reveal) {
        std::vector<tactical::TypeId> types;
        for (const tactical::UnitState& unit : setup.units) types.push_back(unit.type_id);
        content.value().sensors = skirmish::revealed_sensor_table(types);
    }
    std::map<tactical::TypeId, std::string> names;
    for (const units::UnitType& type : tables->units) names.emplace(skirmish::type_id(type.id), type.id);
    // Team colours: the lobby colour of the M2 slot playing the player's faction.
    std::map<tactical::FactionId, std::array<std::uint8_t, 3>> faction_colours;
    if (auto inputs = skirmish::read_start_inputs(skirmish::m2_fixture(), filesystem, catalog, *tables)) {
        // #495: an M2 replay without --eawr-live-reveal runs on the map's fog grid.
        if (!options_.reveal) {
            auto fog = skirmish::fog_rules(inputs.value());
            if (!fog) {
                failure = "live session fog rules: " + core::format_diagnostic(fog.error());
                return false;
            }
            content.value().fog = fog.value();
        }
        for (const skirmish::LobbySlot& slot : skirmish::m2_fixture().slots) {
            if (slot.slot >= 1U && slot.slot - 1U < inputs.value().lobby_colours.size()) {
                faction_colours.emplace(skirmish::faction_id(slot.faction), inputs.value().lobby_colours[slot.slot - 1U].rgb);
            }
        }
        // #501: map-object units (e.g. Skirmish_Merchant_Dock) are not in the unit tables; the
        // live path places them straight from the map's placements (add_map_objects), so a
        // replay resolves their type the same way instead of a second lookup.
        for (const auto& [id, type] : skirmish::map_object_type_names(inputs.value().placements)) names.emplace(id, type);
        // #530: a replay of the M2 start runs with its economy, as sim_headless does.
        auto start = skirmish::build_start(skirmish::m2_fixture(), inputs.value());
        if (start && start.value().setup.players == setup.players && start.value().setup.units == setup.units) {
            auto economy = skirmish::economy_rules(start.value(), inputs.value(), *tables);
            if (!economy) {
                failure = "live session economy: " + core::format_diagnostic(economy.error());
                return false;
            }
            economy_ = std::move(economy).value();
        }
    }
    player_ = options_.player.value_or(setup.players.empty() ? 1U : setup.players.front().player_id);
    if (std::none_of(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& player) { return player.player_id == player_; })) {
        failure = "--eawr-live-player " + std::to_string(player_) + " is not a player of the replay";
        return false;
    }
    for (const tactical::Player& player : setup.players) team_of_player_[player.player_id] = player.team_id;
    for (const tactical::UnitState& unit : setup.units) {
        const auto name = names.find(unit.type_id);
        if (name == names.end()) {
            failure = "--eawr-live-replay: unit " + std::to_string(unit.entity_id) + " has a type the tables lack";
            return false;
        }
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = name->second;
        ship.position = {to_float(unit.position.x), to_float(unit.position.y), to_float(unit.position.z)};
        auto transform = sim::math::to_matrix(unit.rotation, unit.position);
        ship.yaw_degrees = transform ? static_cast<float>(space::instance_yaw_degrees(transform.value())) : 0.0F;
        ship.live_entity = unit.entity_id;
        const auto owner = std::find_if(setup.players.begin(), setup.players.end(),
            [&](const tactical::Player& player) { return player.player_id == unit.owner; });
        if (owner != setup.players.end()) {
            if (const auto colour = faction_colours.find(owner->faction_id); colour != faction_colours.end()) {
                ship.team_colour = colour->second;
            }
        }
        ship_of_entity_.emplace(unit.entity_id, placed_ships_.size());
        owner_of_entity_.emplace(unit.entity_id, unit.owner);
        placed_ships_.push_back(std::move(ship));
    }
    setup_ = setup;
    // #77: like sim_headless, the replay's human players are the pinned fixture's human slots.
    victory_ = skirmish::victory_rules(setup, *tables, skirmish::human_slots(skirmish::m2_fixture()));
    replay_ = std::move(replay).value();
    content_ = std::move(content).value();
    tables_ = std::move(*tables);
    return true;
}

bool LiveSessionView::prepare(const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view map_path,
                              std::string& failure) {
    const skirmish::Fixture& fixture = skirmish::m2_fixture();
    if ((options_.fixture == "replay") != !options_.replay_input.empty()) {
        failure = "--eawr-live-replay goes with --eawr-live-session replay, which needs it";
        return false;
    }
    if ((options_.fixture == "m2" || options_.fixture == "melee") && lower_path(map_path) != lower_path(fixture.map)) {
        failure = "--eawr-live-session " + options_.fixture + " runs on " + fixture.map;
        return false;
    }
    if (options_.fixture != "skirmish" && (options_.skirmish.map || options_.skirmish.slots || options_.skirmish.seed)) {
        failure = "--eawr-skirmish-* options require --eawr-live-session skirmish";
        return false;
    }
    if (options_.fixture == "skirmish") {
        if (options_.skirmish.map && *options_.skirmish.map != lower_path(map_path)) {
            failure = "--eawr-map and --eawr-skirmish-map must select the same map";
            return false;
        }
        options_.skirmish.map = lower_path(map_path);
    }
    if (options_.fixture != "melee" && (options_.melee_size || options_.melee_seed)) {
        failure = "--eawr-live-melee and --eawr-live-melee-seed go with --eawr-live-session melee";
        return false;
    }
    if (options_.fixture != "m2" && options_.fixture != "skirmish" && options_.ai.has_value()) {
        ai_flag_ignored_ = true;
        godot::UtilityFunctions::printerr(godot::String(
            ("--eawr-live-ai has no effect with --eawr-live-session " + options_.fixture + "; it only runs the AI"
             + " of m2 or skirmish starts").c_str()));
    }
    if (!(options_.fixture == "replay" || options_.fixture == "melee" ? prepare_replay(filesystem, catalog, failure)
                                                                      : prepare_m2(filesystem, catalog, failure))) {
        return false;
    }
    // #427: a type with a DEFEND ability composes its SHIELD sub-object as its shield shell.
    for (SpacePopulation::Options::PlacedShip& ship : placed_ships_) {
        const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
                                       [&](const units::UnitType& entry) { return entry.id == ship.object_id; });
        ship.defend_shell = type != tables_->units.end()
            && std::any_of(type->abilities.begin(), type->abilities.end(),
                           [](const units::Ability& ability) { return ability.type == "DEFEND"; });
    }
    // #76 AB-31 (UA-06): a type with SPOILER_LOCK plays its DEPLOY clip when the ability switches
    // on and UNDEPLOY when it switches off. Retail draws among several variants; the X-wing has one.
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    const auto prepare_sfoil_clips = [&] {
        for (SpacePopulation::Options::PlacedShip& ship : placed_ships_) {
            const auto type = std::find_if(tables_->units.begin(), tables_->units.end(),
                                           [&](const units::UnitType& entry) { return entry.id == ship.object_id; });
            if (!ship.clip.empty() || type == tables_->units.end()
                || std::none_of(type->abilities.begin(), type->abilities.end(),
                                [](const units::Ability& ability) { return ability.type == "SPOILER_LOCK"; })) {
                continue;
            }
            auto object = catalog.resolve(ship.object_id);
            if (!object) continue;
            const std::string model = space_model_path(object.value());
            const std::string anim_override = tag_text(object.value(), "Space_Model_Anim_Override_Name");
            const auto deploy = animation::model_clip_paths(model, animation::clip_types::deploy, exists, anim_override);
            const auto undeploy = animation::model_clip_paths(model, animation::clip_types::undeploy, exists, anim_override);
            if (deploy.empty() || undeploy.empty()) continue;
            ship.clip = deploy.front();
            ship.alternate_clip = undeploy.front();
        }
    };
    prepare_sfoil_clips();
    prepare_clips(filesystem, catalog);
    prepare_launch_slots(filesystem, catalog);
    // #614: a squadron a hangar launches later flies in a launch slot, which the first pass
    // did not see; without the clips the slot's craft never plays DEPLOY or UNDEPLOY.
    prepare_sfoil_clips();
    // #424: the setup's squadrons select and order as one unit, their team container.
    if (setup_) {
        for (const tactical::Squadron& squadron : setup_->squadrons) register_squadron(squadron, 0);
    }
    for (const ScriptedInput& input : options_.inputs) {
        // #518: an icon may be a squadron a spawner launches later, which the start does not hold;
        // a target that never shows is logged as not on screen.
        if (input.unit && !input.icon && !ship_of_entity_.contains(*input.unit)) {
            failure = "--eawr-live-input: unit " + std::to_string(*input.unit) + " is not a unit of the start";
            return false;
        }
    }
    // --eawr-live-late-orders: a unit the start does not hold (bought or launched later) has an ID
    // above every start unit's; an order on one is given as the local player, whose purchases they are.
    sim::EntityId last_start_unit = 0;
    for (const auto& [entity, ship] : ship_of_entity_) last_start_unit = std::max(last_start_unit, entity);
    for (const ScheduledOrder& order : options_.orders) {
        const auto late = [&](const sim::EntityId id) {
            return ship_of_entity_.contains(id) || (options_.late_orders && id > last_start_unit);
        };
        const auto stranger = std::find_if(order.more.begin(), order.more.end(),
            [&](const sim::EntityId id) { return !late(id); });
        if (!late(order.unit) || stranger != order.more.end()) {
            failure = "--eawr-live-order: unit " + std::to_string(stranger != order.more.end() ? *stranger : order.unit)
                + " is not a unit of the start";
            return false;
        }
        if (order.target != 0 && !ship_of_entity_.contains(order.target)) {
            failure = "--eawr-live-order: target " + std::to_string(order.target) + " is not a unit of the start";
            return false;
        }
    }
    // A driven frame shows the tick frame * step; a capture tick between two frames would be
    // drawn at a later tick than its file name says (#316 review 3).
    if (options_.stall && options_.real_time) {
        failure = "--eawr-live-stall is for driven captures";
        return false;
    }
    if (!options_.real_time) {
        for (const std::uint64_t tick : options_.capture_ticks) {
            // Past the stall a frame shows its step's tick plus the skipped ticks.
            std::uint64_t stepped = tick;
            if (options_.stall && (options_.stall->start || tick > options_.stall->tick)) {
                // The first frame presents tick <ticks> after a start stall.
                if (options_.stall->start ? tick < options_.stall->ticks
                                          : tick <= options_.stall->tick + options_.stall->ticks) {
                    failure = "--eawr-live-capture-ticks: tick " + std::to_string(tick) + " falls in the --eawr-live-stall skip";
                    return false;
                }
                stepped = tick - options_.stall->ticks;
            }
            const double frames = std::round(static_cast<double>(stepped) / options_.ticks_per_frame);
            if (std::abs(frames * options_.ticks_per_frame - static_cast<double>(stepped)) > capture_tolerance) {
                std::ostringstream message;
                message << "--eawr-live-capture-ticks: tick " << tick << " falls between frames at --eawr-live-step "
                        << options_.ticks_per_frame << "; a capture tick must be a multiple of the step";
                failure = message.str();
                return false;
            }
        }
    }
    return true;
}

void LiveSessionView::prepare_launch_slots(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    // Past the debris props (max / 2 down) and the death clones (max down).
    constexpr sim::EntityId first_slot_entity = std::numeric_limits<sim::EntityId>::max() / 4U;
    if (!start_ || !tables_) return;
    const auto add_slot = [&](const std::string& model, const skirmish::StartUnit& near,
                              const std::optional<skirmish::LobbyColour>& colour) {
        SpacePopulation::Options::PlacedShip ship;
        ship.object_id = model;
        ship.position = {to_float(near.state.position.x), to_float(near.state.position.y), to_float(near.state.position.z)};
        ship.yaw_degrees = to_float(near.yaw_degrees);
        ship.live_entity = first_slot_entity + launch_slots_.size();
        ship.launch_slot = true;
        if (colour) ship.team_colour = colour->rgb;
        launch_slots_.push_back({skirmish::type_id(model), placed_ships_.size(), false, std::nullopt});
        placed_ships_.push_back(std::move(ship));
    };
    for (const skirmish::Launch& launch : start_->launches) {
        const units::UnitType* squadron = tables_->find(launch.squadron);
        const auto spawner = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.entity_id == launch.spawner; });
        if (squadron == nullptr || spawner == start_->units.end() || launch.count <= 0 || launch.count > 64) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == launch.owner; });
        const std::optional<skirmish::LobbyColour> colour =
            owner != start_->players.end() ? owner->colour : std::optional<skirmish::LobbyColour>{};
        for (std::int32_t index = 0; index < launch.count; ++index) {
            for (const units::SquadronMember& member : squadron->members) add_slot(member.craft, *spawner, colour);
        }
    }
    // #530: the units a human player can buy (PU-10) enter after tick zero too, so they get slots
    // composed with the start: per available unit of its faction's menus, as many as fit its
    // population cap, at most purchase_slots_per_type (space-purchasing PU-G25). Each slot is the
    // unit's model, or for a squadron each craft's. The AI does not buy in M2 (PU-G12). A dead
    // unit's slot is reused (release_dead_slots), so the count bounds what stands at once, not how
    // many were ever bought.
    const std::uint32_t purchase_slots_per_type = options_.purchase_slots;
    for (const tactical::EconomyPlayer& buyer : economy_.players) {
        if (buyer.ai) continue;
        const auto owner = std::find_if(start_->players.begin(), start_->players.end(),
            [&](const skirmish::StartPlayer& player) { return player.player.player_id == buyer.player; });
        const auto near = std::find_if(start_->units.begin(), start_->units.end(),
            [&](const skirmish::StartUnit& unit) { return unit.state.owner == buyer.player; });
        if (owner == start_->players.end() || near == start_->units.end()) continue;
        std::vector<tactical::TypeId> seen;
        for (const tactical::StationMenu& menu : economy_.menus) {
            if (menu.faction != owner->player.faction_id) continue;
            for (const tactical::BuildOption& option : menu.options) {
                if (!option.available || option.kind != tactical::BuildKind::unit) continue;
                if (std::find(seen.begin(), seen.end(), option.type) != seen.end()) continue;
                seen.push_back(option.type);
                const auto named = std::find_if(tables_->units.begin(), tables_->units.end(),
                    [&](const units::UnitType& type) { return skirmish::type_id(type.id) == option.type; });
                if (named == tables_->units.end()) continue;
                // WR-12/13: separate preloaded visual clones, never launch slots or live entities.
                if (buyer.player == player_) {
                    const auto add_preview = [&](const std::string& craft, const sim::math::Vec3 offset) {
                        SpacePopulation::Options::PlacedShip preview;
                        preview.object_id = craft;
                        preview.live_entity = std::numeric_limits<sim::EntityId>::max() / 8U + placement_clones_.size();
                        preview.placement_preview = true;
                        preview.launch_slot = true; // starts hidden until an explicit preview pose
                        if (owner->colour) preview.team_colour = owner->colour->rgb;
                        const auto* footprint = economy_.footprint_of(skirmish::type_id(craft));
                        placement_clones_.push_back({option.type, placed_ships_.size(), offset, footprint ? footprint->layer_z : sim::math::Fixed{}});
                        placed_ships_.push_back(std::move(preview));
                    };
                    if (named->members.empty()) add_preview(named->id, {});
                    for (const auto& member : named->members) add_preview(member.craft, member.offset.value_or(sim::math::Vec3{}));
                }
                const std::uint32_t fits = option.population == 0 ? purchase_slots_per_type : buyer.population_cap / option.population;
                for (std::uint32_t index = 0; index < std::min(fits, purchase_slots_per_type); ++index) {
                    if (named->members.empty()) add_slot(named->id, *near, owner->colour);
                    for (const units::SquadronMember& member : named->members) add_slot(member.craft, *near, owner->colour);
                }
            }
        }
    }
    // #458: each slot's death clone, set up like a start unit's (the clones used to be planned
    // for the start units only, so a craft launched after tick zero died without one). The
    // clone's clip variant is drawn from the slot's own entity, as the craft's ID is not known
    // until it launches.
    for (LaunchSlot& slot : launch_slots_) {
        if (auto clone = prepare_death_clone(filesystem, catalog, slot.ship, placed_ships_[slot.ship].live_entity)) {
            slot.clone = launch_clones_.size();
            launch_clones_.push_back(std::move(*clone));
        }
    }
}

void LiveSessionView::register_squadron(const tactical::Squadron& squadron, const std::uint64_t tick) {
    if (!squadrons_.emplace(squadron.container, squadron).second) return;
    squadron_seen_.emplace(squadron.container, tick);
    squadron_members_[squadron.container] = squadron.members;
    for (const sim::EntityId member : squadron.members) squadron_of_[member] = squadron.container;
}

void LiveSessionView::release_dead_slots(const tactical::TacticalSnapshot& latest) {
    const auto instances = latest.instances();
    const auto standing = [&](const sim::EntityId entity) {
        const auto found = std::lower_bound(instances.begin(), instances.end(), entity,
            [](const tactical::TacticalInstance& instance, const sim::EntityId id) { return instance.entity_id < id; });
        return found != instances.end() && found->entity_id == entity;
    };
    const auto cloned = [&](const sim::EntityId entity) {
        const auto is = [&](const ActiveClone& clone) { return clone.unit == entity; };
        return std::any_of(active_clones_.begin(), active_clones_.end(), is)
            || std::any_of(retiring_clones_.begin(), retiring_clones_.end(), is);
    };
    for (auto bound = launched_ship_of_entity_.begin(); bound != launched_ship_of_entity_.end();) {
        if (standing(bound->first) || cloned(bound->first)) {
            ++bound;
            continue;
        }
        for (LaunchSlot& slot : launch_slots_) {
            if (slot.ship == bound->second) slot.bound = false;
        }
        death_clones_.erase(bound->first);
        ++slots_released_;
        bound = launched_ship_of_entity_.erase(bound);
    }
}

std::optional<std::size_t> LiveSessionView::ship_of(const sim::EntityId entity, const tactical::TypeId type) {
    if (const auto found = ship_of_entity_.find(entity); found != ship_of_entity_.end()) return found->second;
    if (const auto found = launched_ship_of_entity_.find(entity); found != launched_ship_of_entity_.end()) return found->second;
    for (LaunchSlot& slot : launch_slots_) {
        if (slot.bound || slot.type != type) continue;
        slot.bound = true;
        launched_ship_of_entity_.emplace(entity, slot.ship);
        if (slot.clone) death_clones_.emplace(entity, launch_clones_[*slot.clone]);
        return slot.ship;
    }
    return std::nullopt;
}

void LiveSessionView::prepare_clips(const vfs::Vfs& filesystem, const data::Catalog& catalog) {
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    const auto clip_counts = [&](const std::string& model, const std::string& anim_override) {
        std::string counts = "{";
        for (std::size_t index = 0; index < reported_clip_types.size(); ++index) {
            const std::size_t type = reported_clip_types[index];
            counts += (index ? ", " : "") + json(animation::clip_type_names[type]) + ": "
                + std::to_string(animation::model_clip_paths(model, type, exists, anim_override).size());
        }
        return counts + "}";
    };
    std::map<std::string, bool> reported;
    const std::size_t units = placed_ships_.size();
    // The session's units are the first placed ships; an m2 start lists them in the same order
    // (with their fighters' craft type), a replay only by type.
    for (std::size_t index = 0; index < units; ++index) {
        const std::string unit_type = placed_ships_[index].object_id;
        const sim::EntityId entity = placed_ships_[index].live_entity;
        const std::string craft_type = start_ && index < start_->units.size() ? start_->units[index].craft_type : "";
        for (const std::string& type : {unit_type, craft_type}) {
            if (type.empty() || reported[type]) continue;
            reported[type] = true;
            auto object = catalog.resolve(type);
            if (!object) continue;
            const std::string model = space_model_path(object.value());
            std::string row = "{\"type\": " + json(type) + ", \"model\": " + json(model) + ", \"clips\": "
                + (model.empty() ? std::string("null")
                                 : clip_counts(model, tag_text(object.value(), "Space_Model_Anim_Override_Name")));
            const auto clone_type = death_clone_of(object.value());
            row += ", \"death_clone\": " + (clone_type ? json(*clone_type) : std::string("null"));
            if (clone_type) {
                if (auto clone = catalog.resolve(*clone_type)) {
                    const std::string clone_model = space_model_path(clone.value());
                    row += ", \"clone_model\": " + json(clone_model) + ", \"clone_clips\": "
                        + clip_counts(clone_model, tag_text(clone.value(), "Space_Model_Anim_Override_Name"));
                }
            }
            unit_clip_rows_.push_back(row + "}");
        }

        // The unit's death clone, its clip variant drawn from the unit's ID (UA-P2).
        if (auto death = prepare_death_clone(filesystem, catalog, index, entity)) {
            death_clones_.emplace(entity, std::move(*death));
        }
    }
}

std::optional<LiveSessionView::DeathClone> LiveSessionView::prepare_death_clone(const vfs::Vfs& filesystem,
    const data::Catalog& catalog, const std::size_t source, const std::uint64_t variant_key) {
    const auto exists = [&](const std::string& path) { return static_cast<bool>(filesystem.stat(path)); };
    const std::string unit_type = placed_ships_[source].object_id;
    const sim::EntityId entity = placed_ships_[source].live_entity;
    // Its type by the Death_Clone rule, its Specific_Death_Anim_Type clip (UA-08).
    auto object = catalog.resolve(unit_type);
    if (!object) return std::nullopt;
    const auto clone_type = death_clone_of(object.value());
    if (!clone_type) return std::nullopt;
    auto clone = catalog.resolve(*clone_type);
    std::string status = "ready";
    std::string clip_type(animation::clip_type_names[animation::clip_types::die]);
    bool removed = false;
    DeathClone death{.ship = placed_ships_.size(), .type = *clone_type};
    if (!clone) {
        status = "clone type not in the catalog";
    } else {
        const std::string declared = options_.death_anim_type.empty()
            ? tag_text(clone.value(), "Specific_Death_Anim_Type") : options_.death_anim_type;
        std::size_t type = animation::clip_types::die;
        if (!declared.empty()) {
            if (const auto found = animation::clip_type_index(declared)) {
                type = *found;
            } else {
                status = "Specific_Death_Anim_Type " + declared + " is no clip type; DIE is used (unverified)";
            }
        }
        clip_type = animation::clip_type_names[type];
        std::optional<std::uint32_t> declared_index;
        if (const std::string text = tag_text(clone.value(), "Specific_Death_Anim_Index"); !text.empty()) {
            if (const auto value = parse_u64(text); value && *value <= 0xFFFFU) {
                // 0xffff is retail's "draw one" (UC-E9).
                if (*value != 0xFFFFU) declared_index = static_cast<std::uint32_t>(*value);
            }
        }
        const std::string clone_model = space_model_path(clone.value());
        const auto clips = animation::model_clip_paths(clone_model, type, exists,
            tag_text(clone.value(), "Space_Model_Anim_Override_Name"));
        // Retail draws an undeclared variant with the synchronized random generator; the view
        // takes it from the unit's ID, so every run shows the same one (UA-P2).
        const auto variant = animation::death_clip_variant(clips.size(), declared_index, variant_key);
        if (variant) death.clip = clips[*variant];
        const std::string remove = tag_text(clone.value(), "Remove_Upon_Death");
        death.remove_upon_death = iequal(remove, "true") || iequal(remove, "yes");
        if (!variant) {
            if (animation::death_start(false, death.remove_upon_death) == animation::DeathStart::removed) {
                removed = true;
                status = "no " + clip_type + " clip: removed at once (Remove_Upon_Death)";
            } else {
                status = "no " + clip_type + " clip: the clone keeps its pose";
            }
        }
        death.playback.blend_ticks = static_cast<std::uint32_t>(
            animation::seconds_to_ticks(0.5, tactical::logical_frames_per_second).value_or(0U));
        const double persistence = options_.death_persistence.value_or(
            seconds_tag(clone.value(), "Death_Persistence_Duration", -1.0));
        if (persistence >= 0.0) {
            death.playback.persistence_ticks = animation::seconds_to_ticks(persistence, tactical::logical_frames_per_second);
        }
        death.playback.fade_ticks = animation::seconds_to_ticks(
            seconds_tag(clone.value(), "Death_Fade_Time", 0.25), tactical::logical_frames_per_second).value_or(0U);
    }
    death_clone_rows_.push_back("{\"unit\": " + std::to_string(entity) + ", \"type\": "
        + json(unit_type) + ", \"clone\": " + json(*clone_type) + ", \"clip_type\": " + json(clip_type)
        + ", \"clip\": " + json(death.clip) + ", \"remove_upon_death\": "
        + (death.remove_upon_death ? "true" : "false") + ", \"persistence_ticks\": "
        + (death.playback.persistence_ticks ? std::to_string(*death.playback.persistence_ticks) : "null")
        + ", \"status\": " + json(status) + "}");
    if (!clone || removed) return std::nullopt;
    SpacePopulation::Options::PlacedShip ship;
    ship.object_id = *clone_type;
    ship.position = placed_ships_[source].position;
    ship.yaw_degrees = placed_ships_[source].yaw_degrees;
    // The clone is its own live ship, never a session unit.
    ship.live_entity = std::numeric_limits<sim::EntityId>::max() - entity;
    ship.team_colour = placed_ships_[source].team_colour;
    ship.colour_record = placed_ships_[source].colour_record;
    ship.clip = death.clip;
    ship.death_clone = true;
    placed_ships_.push_back(std::move(ship));
    return death;
}

bool LiveSessionView::start(std::string& failure) {
    if (!setup_ || !content_) {
        failure = "the live session was not prepared";
        return false;
    }
    platform::LiveSession::Options session_options;
    session_options.workers = options_.workers.value_or(platform::LiveSession::game_worker_count());
    session_options.pacing = options_.real_time ? platform::LiveSession::Pacing::real_time
                                                : platform::LiveSession::Pacing::driven;
    session_options.target_rate = time_.target_rate(); // #459 TM-01
    // UI-07: the local player's one scheduler, taken on the simulation thread right before
    // each step with its own keys. The session starts at tick 0.
    scheduler_ = std::make_unique<ui::CommandScheduler>(player_, 0, 0);
    order_input_ = std::make_unique<ui::OrderInput>(*scheduler_);
    session_options.scripts = ai_scripts_;
    // #494: the local player's fog cells ride along with each tick for the fog in the world.
    if (content_->fog) session_options.fog_player = player_;
    session_options.command_source = [scheduler = scheduler_.get(), fault = options_.fault_tick,
                                      replay = replay_ ? &*replay_ : nullptr](const std::uint64_t next_tick) {
        if (fault && next_tick == *fault) {
            throw std::runtime_error("--eawr-live-fault-tick injected a fault before tick " + std::to_string(next_tick));
        }
        // A replay session's recorded commands run at their ticks with their keys, before the
        // local player's.
        std::vector<tactical::PlayerCommand> commands;
        if (replay != nullptr) {
            for (const tactical::PlayerCommand& command : replay->commands) {
                if (command.key.tick == next_tick) commands.push_back(command);
            }
        }
        auto taken = scheduler->take(next_tick);
        commands.insert(commands.end(), std::make_move_iterator(taken.begin()), std::make_move_iterator(taken.end()));
        return commands;
    };
    auto live = platform::LiveSession::start(*setup_, content_->sensors, content_->durability, content_->motion,
                                             content_->combat, session_options, victory_, content_->fog, content_->abilities,
                                             economy_);
    if (!live) {
        failure = "live session: " + core::format_diagnostic(live.error());
        return false;
    }
    session_ = std::move(live).value();
    // The debug hook: each order as its unit's owner would give it, at its tick.
    for (const ScheduledOrder& order : options_.orders) {
        const auto owner = owner_of_entity_.find(order.unit);
        platform::LiveOrder live_order{owner != owner_of_entity_.end() ? owner->second : player_, {order.unit},
                                       tactical::StopPayload{}, order.tick};
        live_order.units.insert(live_order.units.end(), order.more.begin(), order.more.end());
        if (order.kind == tactical::OrderKind::move) live_order.payload = tactical::MovePayload{order.point};
        if (order.kind == tactical::OrderKind::face) live_order.payload = tactical::FacePayload{order.point};
        if (order.kind == tactical::OrderKind::damage) {
            live_order.payload = tactical::DamagePayload{order.amount, order.hardpoint};
        }
        if (order.kind == tactical::OrderKind::attack) live_order.payload = tactical::AttackPayload{order.target};
        if (order.kind == tactical::OrderKind::ability) {
            tactical::AbilityPayload ability{order.ability, order.action};
            ability.target = order.target; // #561: none unless the order names one
            live_order.payload = ability;
        }
        if (order.kind == tactical::OrderKind::attack_move) {
            live_order.payload = tactical::AttackMovePayload{order.point, order.target};
        }
        if (order.kind == tactical::OrderKind::guard) live_order.payload = tactical::GuardPayload{order.point, order.target};
        session_->submit(std::move(live_order));
    }
    return true;
}

core::Result<SpaceLiveUpdate> LiveSessionView::frame(SpacePopulation& population, GodotRenderer& renderer,
                                                     const double delta) {
    using UpdateResult = core::Result<SpaceLiveUpdate>;
    const auto fail = [](std::string message) {
        return UpdateResult::failure({.code = "EAWR-VIEWER-LIVE-SESSION", .message = std::move(message)});
    };
    // The simulation thread stopped: keep the last poses and carry its error to the view.
    const auto stopped = [this](const core::Diagnostic& failure) {
        simulation_error_ = core::format_diagnostic(failure);
        save_failure_replay();
        SpaceLiveUpdate update;
        update.error = simulation_error_ + "\n"
            + (failure_replay_.empty() ? "No replay saved: " + failure_replay_error_ : "Replay saved: " + failure_replay_);
        return UpdateResult::success(std::move(update));
    };
    if (!session_) return fail("the live session is not running");
    if (const auto failure = session_->failure()) return stopped(*failure);
    ++frames_;
    std::shared_ptr<const tactical::TacticalSnapshot> previous;
    std::shared_ptr<const tactical::TacticalSnapshot> latest;
    double alpha{};
    const std::optional<std::uint64_t> halt = session_->halt_tick();
    if (options_.real_time) {
        // Up to one tick behind the session: the presented tick follows it at the time panel's
        // rate (TM-02's whole-millisecond waits) and holds while paused (TM-07, TP-01).
        const platform::LiveFrame frame = session_->frame();
        previous = frame.previous;
        latest = frame.latest;
        const auto newest = static_cast<double>(latest->completed_tick());
        const double rate = time_.running() ? 1000.0 / static_cast<double>(1000U / time_.target_rate()) : 0.0;
        const double advance = std::isfinite(delta) && delta > 0.0 ? delta * rate : 0.0;
        presented_tick_ = std::clamp(presented_tick_ + advance, std::max(0.0, newest - 1.0), newest);
        alpha = presented_tick_ - (newest - 1.0);
    } else {
        const std::uint64_t shown = frames_ > options_.warmup_frames ? frames_ - 1U - options_.warmup_frames : 0U;
        // TP-06: the presented tick advances --eawr-live-step x target / 30 per frame, not at all
        // while paused; a change starts a new segment at the tick the last frame showed.
        const double factor = time_.tick_factor();
        if (factor != driven_factor_) {
            driven_origin_tick_ = driven_base_;
            driven_origin_frame_ = last_shown_;
            driven_factor_ = factor;
        }
        driven_base_ = driven_origin_tick_
            + static_cast<double>(shown - driven_origin_frame_) * options_.ticks_per_frame * driven_factor_;
        last_shown_ = shown;
        presented_tick_ = driven_base_;
        if (options_.stall
            && (options_.stall->start || presented_tick_ > static_cast<double>(options_.stall->tick) + capture_tolerance)) {
            presented_tick_ += static_cast<double>(options_.stall->ticks);
        }
        // BEP-02: the session halts at end_tick; the frames hold there.
        if (halt) presented_tick_ = std::min(presented_tick_, static_cast<double>(*halt));
        auto base = static_cast<std::uint64_t>(std::floor(presented_tick_ + 1.0e-9));
        alpha = std::max(0.0, presented_tick_ - static_cast<double>(base));
        if (halt && base + 1U > *halt && *halt > 0U) {
            base = *halt - 1U;
            alpha = 1.0;
        }
        session_->advance_to(base + 1U);
        if (!session_->wait_for(base + 1U, std::chrono::seconds(60))) {
            if (const auto failure = session_->failure()) return stopped(*failure);
            return fail("the simulation thread did not reach tick " + std::to_string(base + 1U) + " within 60 s");
        }
        previous = session_->snapshot_at(base);
        latest = session_->snapshot_at(base + 1U);
        if (!previous || !latest) return fail("tick " + std::to_string(base) + " left the snapshot history");
    }
    FrameTimer bookkeeping_timer(trace_frames_ ? &bookkeeping_ms_ : nullptr);
    // #518: a squadron a spawner launched registers like a tick-zero one (selection, icon,
    // dogfight grid, leader lookup). A frame may skip ticks, so both ends of it are read.
    for (const auto* snapshot : {previous.get(), latest.get()}) {
        if (snapshot == nullptr) continue;
        for (const tactical::Squadron& squadron : snapshot->squadrons()) register_squadron(squadron, snapshot->completed_tick());
    }
    // The events of the ticks this frame reached, from the session's event log rather than the
    // snapshot history, so a stall longer than the history loses none (#370 review 3). Should
    // the log's own bound have dropped some, the gap is reported, never silently skipped.
    battle_frame_.reached.clear();
    if (latest->completed_tick() > reached_tick_) {
        platform::LiveEvents reached = session_->events_after(reached_tick_, latest->completed_tick());
        if (reached.lost_through) {
            event_gap_rows_.push_back("{\"after\": " + std::to_string(reached_tick_) + ", \"through\": "
                                      + std::to_string(*reached.lost_through) + "}");
            const std::string message = "live session: the events of ticks " + std::to_string(reached_tick_ + 1U)
                + " to " + std::to_string(*reached.lost_through)
                + " left the event log before a frame reached them; their hits and deaths are not shown";
            godot::UtilityFunctions::printerr(godot::String(message.c_str()));
        }
        battle_frame_.reached = std::move(reached.ticks);
    }
    for (const platform::LiveTickEvents& record : battle_frame_.reached) {
        for (const tactical::CombatEvent& event : record.combat_events) {
            if (event.kind != tactical::CombatEventKind::projectile_hit || first_hits_.size() >= first_hits_limit) continue;
            first_hits_.emplace(std::make_pair(event.shooter, event.target), event.tick);
        }
    }
    // #81: the units destroyed since the last frame's newest tick hand over to their death
    // clones, at the pose the unit was last drawn with (a unit the local player did not see
    // leaves no clone). #447: a craft that spins away keeps its ship along the spin and hands
    // over when the spin ends, at its last spin pose.
    for (const platform::LiveTickEvents& record : battle_frame_.reached) {
        for (const tactical::Event& event : record.events) {
            if (event.kind == tactical::EventKind::spin_away_started) {
                spin_rows_[event.unit] = SpinRow{record.tick, std::nullopt};
                continue;
            }
            if (event.kind == tactical::EventKind::spin_away_ended) {
                if (const auto row = spin_rows_.find(event.unit); row != spin_rows_.end()) row->second.ended = record.tick;
            } else if (event.kind != tactical::EventKind::unit_destroyed) {
                continue;
            }
            if (event.kind == tactical::EventKind::unit_destroyed
                && std::any_of(record.events.begin(), record.events.end(), [&event](const tactical::Event& other) {
                       return other.kind == tactical::EventKind::spin_away_started && other.unit == event.unit;
                   })) {
                continue;
            }
            const auto clone = death_clones_.find(event.unit);
            const auto pose = last_poses_.find(event.unit);
            if (clone == death_clones_.end() || pose == last_poses_.end()) continue;
            ActiveClone active{event.unit, record.tick, pose->second};
            active.pose.ship = clone->second.ship;
            active.pose.hardpoints.clear();
            active_clones_.push_back(std::move(active));
        }
    }
    latest_tick_ = latest->completed_tick();
    reached_tick_ = std::max(reached_tick_, latest_tick_);
    ability_snapshot_ = latest;
    for (const tactical::TacticalInstance& instance : latest->instances()) {
        if (instance.ion_stun_frames > 0) {
            auto [row, added] = ion_stun_rows_.try_emplace(instance.entity_id, IonStunRow{latest->completed_tick(), 0U});
            static_cast<void>(added);
            row->second.max_frames = std::max(row->second.max_frames, instance.ion_stun_frames);
        }
        if (!squadron_members_.contains(instance.entity_id)) continue;
        for (const tactical::AbilityStatus& status : instance.abilities) {
            if (status.kind != tactical::AbilityKind::ion_cannon_shot) continue;
            IonShotRow& row = ion_shot_rows_[instance.entity_id];
            if (status.active && !row.on) {
                ++row.switched_on;
                if (row.first_on == 0) row.first_on = latest->completed_tick();
            }
            if (status.active) row.last_on = latest->completed_tick();
            row.on = status.active;
        }
    }
    if (!outcome_ && latest->outcome()) {
        outcome_ = latest->outcome();
        // #453 BEP-02: the battle ends at end_tick; BE-02: the winner's team wins, every other
        // lobby team loses.
        session_->halt_at(outcome_->end_tick);
        const auto team = team_of_player_.find(player_);
        const bool won = team != team_of_player_.end() && team->second == outcome_->winner_team;
        battle_end_ = BattleEnd{won ? ui::BattleResult::victory : ui::BattleResult::defeat, outcome_->decided_tick,
                                outcome_->end_tick, shown_frames(), std::nullopt};
    }
    if (battle_end_ && !battle_end_->ended_frame
        && presented_tick_ + capture_tolerance >= static_cast<double>(battle_end_->end_tick)) {
        battle_end_->ended_frame = shown_frames();
        time_.end(battle_end_->end_tick);
        apply_time();
    }
    battle_frame_.previous = previous;
    battle_frame_.latest = latest;
    battle_frame_.alpha = std::clamp(alpha, 0.0, 1.0);
    battle_frame_.presented_tick = presented_tick_;
    battle_frame_.fog = session_->fog_at(latest->completed_tick());
    // #530 (PU-35, PU-36): evidence of each hyperspace arrival the local player saw start: the tick
    // it was first listed, the first tick it was visible to the local team (frame 35) and the tick
    // it had landed (no arrival frame any more).
    for (const tactical::TacticalInstance& instance : latest->instances()) {
        if (!instance.arrival) continue;
        auto& row = arrivals_[instance.entity_id];
        if (row.first_tick == 0) {
            row.first_tick = latest->completed_tick();
            row.owner = instance.owner;
            row.type = instance.type_id;
        }
        row.last_frame = *instance.arrival;
        if (row.visible_tick == 0 && *instance.arrival >= tactical::arrival_visible_frame) row.visible_tick = latest->completed_tick();
    }
    for (auto& [entity, row] : arrivals_) {
        if (row.landed_tick != 0) continue;
        const auto found = std::find_if(latest->instances().begin(), latest->instances().end(),
            [entity = entity](const tactical::TacticalInstance& instance) { return instance.entity_id == entity; });
        if (found != latest->instances().end() && !found->arrival) row.landed_tick = latest->completed_tick();
    }

    // #535: the local player's per-unit fog fade (space-fog-presentation.md FW-16 to FW-18).
    // `visible_now` is exactly what visible_units() and selection keep reading (unchanged); the
    // fade only adds ghosts of units that just left it, still drawn at their true position while
    // they ease out. `--eawr-live-reveal` (#507's draw bypass) shows every instance at full
    // opacity: the fade is not advanced, so nothing ghosts and no opacity is applied (FW-16).
    snapshot_index_.refresh(latest, player_);
    const auto& visible_now = snapshot_index_.visible();
    const auto& alive_now = snapshot_index_.alive();
    const double fade_frames = fade_presented_tick_ ? std::max(0.0, presented_tick_ - *fade_presented_tick_) : 0.0;
    fade_presented_tick_ = presented_tick_;
    if (!options_.reveal) fade_.advance(visible_now, alive_now, fade_frames);
    std::vector<sim::EntityId> fading_entities;
    for (const sim::EntityId entity : fade_.drawn()) {
        if (!std::binary_search(visible_now.begin(), visible_now.end(), entity)) fading_entities.push_back(entity);
    }
    fading_units_ = fading_entities.size();
    // #535: one row a unit that is not at full opacity, for each frame that shows a new newest tick
    // (K-4: a test reads a unit's ramp across several ticks from one report, instead of one run a
    // tick).
    if (latest->completed_tick() != fading_logged_tick_) {
        fading_logged_tick_ = latest->completed_tick();
        const std::uint64_t tick = latest->completed_tick();
        for (const sim::EntityId entity : fade_.drawn()) {
            if (fading_log_rows_.size() >= fading_log_limit) break;
            const auto opacity = fade_.opacity(entity);
            if (!opacity || *opacity >= 1.0F) continue;
            fading_log_rows_.push_back("{\"tick\": " + std::to_string(tick) + ", \"entity\": " + std::to_string(entity)
                + ", \"opacity\": " + std::to_string(*opacity) + "}");
        }
    }

    if (!space::interpolate_visible_units(*previous, *latest, alpha, visible_now, options_.reveal,
                                         fading_entities, poses_, pose_workers_)) {
        return fail("live unit interpolation failed");
    }
    const auto& poses = poses_;
    // #447: the killed craft spinning away that the local player sees.
    const auto spinning = space::interpolate_spinning(*previous, *latest, alpha, player_, options_.reveal);
    unit_frames_.clear();
    for (const auto* drawn : {&poses, &spinning}) {
        for (const space::LiveUnitPose& pose : *drawn) {
            unit_frames_.emplace(pose.entity, BattleEffects::UnitFrame{pose.position, pose.yaw_degrees, pose.type,
                                                                       pose.roll_degrees, pose.pitch_degrees});
        }
    }
    spinning_drawn_max_ = std::max(spinning_drawn_max_, spinning.size());
    const auto with_ship = std::count_if(spinning.begin(), spinning.end(),
        [this](const space::LiveUnitPose& pose) {
            return ship_of_entity_.contains(pose.entity) || launched_ship_of_entity_.contains(pose.entity);
        });
    spinning_ships_max_ = std::max(spinning_ships_max_, static_cast<std::size_t>(with_ship));
    // #82: what the local player can pick this frame, and what still stands. FW-18's fading
    // ghosts are drawn (below) but never selectable or orderable: they are not in visible_now.
    const auto team = [this](const tactical::PlayerId owner) {
        const auto found = team_of_player_.find(owner);
        return found == team_of_player_.end() ? std::optional<tactical::TeamId>{} : found->second;
    };
    release_dead_slots(*latest);
    visible_.clear();
    for (const space::LiveUnitPose& pose : poses) {
        if (!options_.reveal && !std::binary_search(visible_now.begin(), visible_now.end(), pose.entity)) continue;
        const auto ship = ship_of(pose.entity, pose.type);
        if (!ship) continue;
        visible_.push_back({pose.entity, *ship, pose.type, pose.owner, pose.owner == player_,
                            team(pose.owner) != team(player_), pose.position, pose.yaw_degrees});
    }
    auto& live = live_poses_;
    live.clear();
    live.reserve(poses.size() + spinning.size() + active_clones_.size());
    auto& clip_poses = clip_poses_;
    clip_poses.clear();
    for (const auto* drawn : {&poses, &spinning}) {
        for (const space::LiveUnitPose& pose : *drawn) {
            // Spawned after tick zero without a launch slot (a squadron's container): nothing composed.
            const auto ship = ship_of(pose.entity, pose.type);
            if (!ship) continue;
            SpacePopulation::LivePose placed;
            placed.ship = *ship;
            // #506: a squadron craft's pitch too, so it flies nose first (space-fighters FM-02, FM-05).
            std::array<sim::math::Fixed, 6> values{};
            const std::array<double, 6> source{pose.position[0], pose.position[1], pose.position[2], pose.yaw_degrees,
                                               pose.roll_degrees, pose.pitch_degrees};
            for (std::size_t index = 0; index < values.size(); ++index) {
                auto fixed = scene::fixed_from_binary32(static_cast<float>(source[index]));
                if (!fixed) return fail("live unit " + std::to_string(pose.entity) + " pose is not finite");
                values[index] = fixed.value();
            }
            placed.position = {values[0], values[1], values[2]};
            placed.yaw_degrees = values[3];
            placed.roll_degrees = values[4];
            placed.pitch_degrees = values[5];
            // A spinning craft (#447) is dead: no DEFEND shell.
            placed.defend_active = options_.defend && !pose.spinning;
            if (pose.instance != nullptr) {
                for (const tactical::AbilityStatus& ability : pose.instance->abilities) {
                    if (ability.kind == tactical::AbilityKind::defend && ability.active) placed.defend_active = true;
                }
            }
            if (pose.instance != nullptr && pose.instance->durability) {
                for (const tactical::HardpointStatus& hardpoint : pose.instance->durability->hardpoints) {
                    placed.hardpoints.push_back(static_cast<scene::HardpointState>(hardpoint.state));
                }
            }
            // #76 AB-31 (UA-06): the S-foils follow SPOILER_LOCK. A switch starts the other clip at the
            // frame that mirrors the running clip's remaining frames, at the type's deployment rate
            // (1.0 for the X-wing; retail's 1/30 s blend is not drawn), and it holds its last frame.
            // A craft spinning away (#447) keeps the S-foils it died with.
            if (const animation::Player* deploy = population.live_clip(*ship);
                deploy != nullptr && (pose.instance != nullptr || pose.spinning) && population.live_clip(*ship, true) != nullptr) {
                SFoil& foil = sfoils_[pose.entity];
                bool on = foil.on;
                if (pose.instance != nullptr) {
                    on = false;
                    for (const tactical::AbilityStatus& ability : pose.instance->abilities) {
                        if (ability.kind == tactical::AbilityKind::spoiler_lock && ability.active) on = true;
                    }
                }
                const auto frame_of = [&](const SFoil& state) {
                    const animation::Player* player = population.live_clip(*ship, state.alternate);
                    const double frames = player->playable_frames();
                    const double elapsed = std::max(0.0, presented_tick_ - state.since);
                    const double frame = state.start_frame
                        + elapsed * player->frames_per_second() / tactical::logical_frames_per_second;
                    return std::pair{std::min(frame, std::max(0.0, frames - 1.0)), player->playable_frames()};
                };
                if (on != foil.on) {
                    std::int64_t start = 0;
                    if (foil.started) {
                        const auto [frame, count] = frame_of(foil);
                        start = static_cast<std::int64_t>(count) - static_cast<std::int64_t>(frame) - 1;
                    }
                    foil = SFoil{on, true, !on, presented_tick_, static_cast<std::uint32_t>(std::max<std::int64_t>(start, 0))};
                    ++sfoil_switches_;
                }
                if (foil.started) {
                    constexpr std::uint32_t subdivisions = 64;
                    const auto [frame, count] = frame_of(foil);
                    static_cast<void>(count);
                    clip_poses.push_back({*ship, {static_cast<std::uint64_t>(frame * subdivisions), subdivisions}, 0.0F,
                                          foil.alternate});
                }
            }
            last_poses_[pose.entity] = placed;
            live.push_back(std::move(placed));
        }
    }
    std::erase_if(last_poses_, [this](const auto& entry) { return !unit_frames_.contains(entry.first); });
    const std::size_t seen = options_.reveal ? poses.size() : visible_now.size();
    visible_units_ = seen;
    hidden_units_ = latest->instances().size() - seen;
    // A clone shows from the first frame past its unit's last drawn tick, its clip clock
    // counting whole ticks of the presentation clock from there.
    // A clone whose clip did not start (no clip of its type, or it did not bind) is removed at
    // once with Remove_Upon_Death and otherwise keeps its pose; one that has faded out leaves.
    // Either way it is no longer drawn, and its ship is retired and its resources released once
    // the frame's unit emitters have run the samples it still stood in (#429).
    retire_clones(population, renderer);
    for (auto active = active_clones_.begin(); active != active_clones_.end();) {
        const DeathClone& clone = death_clones_.at(active->unit);
        const animation::Player* player = population.live_clip(clone.ship);
        const double elapsed = presented_tick_ - (static_cast<double>(active->death_tick) - 1.0);
        const auto tick = static_cast<std::uint64_t>(std::max(0.0, std::floor(elapsed + 1.0e-9)));
        const animation::DeathStart start = animation::death_start(player != nullptr, clone.remove_upon_death);
        const auto death = clone_death_frame(clone, player, tick);
        if (start == animation::DeathStart::removed || (death && !death->shown)) {
            retired_clone_rows_.push_back("{\"unit\": " + std::to_string(active->unit) + ", \"death_tick\": "
                + std::to_string(active->death_tick) + ", \"clone_tick\": " + std::to_string(tick) + ", \"reason\": "
                + json(start == animation::DeathStart::removed ? "removed: clip did not start" : "faded out") + "}");
            retiring_clones_.push_back(std::move(*active));
            active = active_clones_.erase(active);
            continue;
        }
        if (death) {
            live.push_back(active->pose);
            if (player != nullptr) clip_poses.push_back({clone.ship, death->position, death->blend_from});
        }
        ++active;
    }
    // #391: the breakoff props of the hardpoints destroyed so far.
    if (debris_) {
        debris_->pose(battle_frame_.reached, [this](const std::uint64_t tick) { return session_->snapshot_at(tick); },
                      player_, presented_tick_, live, options_.reveal);
    }
    // #456: the model projectiles in flight, each on its pool's placed ship.
    if (projectile_models_) {
        // #862: the ion shots launched since the last frame pose as the ability shot's model.
        projectile_models_->note_ability_shots(battle_frame_.reached, *latest,
            [this](const std::uint64_t tick) { return session_->snapshot_at(tick); });
        projectile_models_->pose_projectile_models(*previous, *latest, battle_frame_.alpha,
            [this](const sim::EntityId entity) { return unit_frame(entity); }, live);
    }
    // WR-12..14: cursor-following clones use the same composed model, scale and idle reader
    // as ordinary ships. Emitters were removed from their private placements at composition.
    if (preview_type_ && preview_point_ && reinforcement_allowed()) {
        const auto* player = economy_.player(player_);
        if (player != nullptr) {
            const auto direction = tactical::planar_direction(player->reinforcement_yaw);
            if (!direction) return fail(core::format_diagnostic(direction.error()));
            std::ostringstream sample;
            sample << "{\"tick\": " << latest->completed_tick() << ", \"valid\": "
                   << (preview_valid_ ? "true" : "false") << ", \"clones\": [";
            std::size_t clone_index = 0;
            for (const auto& clone : placement_clones_) {
                if (clone.type != *preview_type_) continue;
                const auto xx = sim::math::multiply(clone.offset.x, direction.value().x);
                const auto yy = sim::math::multiply(clone.offset.y, direction.value().y);
                const auto xy = sim::math::multiply(clone.offset.x, direction.value().y);
                const auto yx = sim::math::multiply(clone.offset.y, direction.value().x);
                if (!xx || !yy || !xy || !yx) return fail("reinforcement preview offset overflow");
                SpacePopulation::LivePose placed;
                placed.ship = clone.ship;
                placed.position = {sim::math::Fixed::from_raw(preview_point_->x.raw() + xx.value().raw() - yy.value().raw()),
                    sim::math::Fixed::from_raw(preview_point_->y.raw() + xy.value().raw() + yx.value().raw()),
                    sim::math::Fixed::from_raw(clone.layer_z.raw() + clone.offset.z.raw())};
                placed.yaw_degrees = player->reinforcement_yaw;
                live.push_back(placed);
                sample << (clone_index++ ? ", " : "") << "[" << to_float(placed.position.x) << ", "
                       << to_float(placed.position.y) << ", " << to_float(placed.position.z) << "]";
                for (const auto entity : population.live_ship_entities(clone.ship)) {
                    renderer.set_light_scale(entity, preview_colours_[preview_valid_ ? 0 : 1]);
                    renderer.set_unit_opacity(entity, 1.0F); // authored colour alpha is not opacity
                }
            }
            sample << "]}";
            if (preview_rows_.size() < 256) preview_rows_.push_back(sample.str());
            ++preview_frames_;
        }
    }
    bookkeeping_timer.finish();
    if (!population.pose_live(live)) return fail(population.failure());
    // #535: the fog fade's opacity (FW-16 to FW-19) goes to every piece of the ship; the pieces
    // whose adapters carry the unit's opacity uniform (the hull surfaces, as BP-21's shield flash
    // reaches) dither with it, the others ignore it (G-FW14).
    for (const space::LiveUnitPose& pose : poses) {
        const auto ship = ship_of(pose.entity, pose.type);
        if (!ship) continue;
        const float opacity = fade_.opacity(pose.entity).value_or(1.0F);
        for (const sim::EntityId piece : population.live_ship_entities(*ship)) {
            renderer.set_unit_opacity(piece, opacity);
            // WR-37: frame 35 starts a light fade lasting 115/(FPS*4) seconds.
            // Presentation time interpolates the logical counter; fog opacity stays independent.
            float light = 1.0F;
            if (pose.instance != nullptr && pose.instance->arrival) {
                const double birth = static_cast<double>(latest->completed_tick()) - *pose.instance->arrival;
                light = static_cast<float>(std::clamp((presented_tick_ - birth - tactical::arrival_visible_frame)
                    / (static_cast<double>(tactical::arrival_sweep_frames) / 4.0), 0.0, 1.0));
            }
            renderer.set_light_scale(piece, {light, light, light});
        }
    }
    if (!population.pose_live_clips(renderer, clip_poses)) return fail(population.failure());
    // #427: the shield shells' clock is the presentation clock (seconds).
    population.set_shield_time(renderer, static_cast<float>(presented_tick_ / tactical::logical_frames_per_second));
    // #427/#438 BP-21: opt-in only. A hit the shield took whole starts the colour flash at the tick's
    // start (as its particles are born, BattleEffects), restarting a running one: its own
    // surfaces' light scale RGB starts at Shield_Flash_Scale and returns linearly to 1 over
    // Shield_Flash_Duration. Its hardpoints' attached models are other objects and keep theirs.
    if (options_.shield_flash) {
        for (const platform::LiveTickEvents& record : battle_frame_.reached) {
            for (const tactical::CombatEvent& event : record.combat_events) {
                if (event.kind != tactical::CombatEventKind::projectile_hit
                    || (event.outcome & tactical::hit_outcome_shield_absorbed) == 0U) {
                    continue;
                }
                shield_flash_start_[event.target] = static_cast<double>(record.tick) - 1.0;
                ++shield_flashes_;
            }
        }
    }
    for (auto flash = shield_flash_start_.begin(); flash != shield_flash_start_.end();) {
        const double seconds = (presented_tick_ - flash->second) / tactical::logical_frames_per_second;
        const bool running = seconds >= 0.0 && seconds < shield_flash_duration;
        std::array<float, 3> scale{1.0F, 1.0F, 1.0F};
        for (std::size_t channel = 0; running && channel < 3; ++channel) {
            const double t = seconds / shield_flash_duration;
            scale[channel] = static_cast<float>(shield_flash_scale[channel] + (1.0 - shield_flash_scale[channel]) * t);
        }
        if (const auto ship = ship_of_entity_.find(flash->first); ship != ship_of_entity_.end()) {
            for (const sim::EntityId entity : population.live_hull_entities(ship->second)) {
                renderer.set_light_scale(entity, scale);
            }
        }
        flash = running ? std::next(flash) : shield_flash_start_.erase(flash);
    }

    SpaceLiveUpdate update;
    update.instances = population.instances();
    if (!options_.real_time) {
        // The clock holds at tick 0 through the warm-up frames; nothing is captured before
        // they have drawn. prepare() admits only capture ticks a frame shows, so the frame that
        // reaches one shows exactly that tick, which names the file.
        if (frames_ > options_.warmup_frames && next_capture_ < options_.capture_ticks.size()) {
            const auto wanted = static_cast<double>(options_.capture_ticks[next_capture_]);
            if (presented_tick_ > wanted + capture_tolerance) {
                return fail("capture tick " + std::to_string(options_.capture_ticks[next_capture_])
                            + " was not shown by any frame");
            }
            if (presented_tick_ + capture_tolerance >= wanted) {
                char suffix[32];
                std::snprintf(suffix, sizeof(suffix), "_t%04llu",
                              static_cast<unsigned long long>(options_.capture_ticks[next_capture_]));
                update.capture_suffix = suffix;
                ++next_capture_;
            }
        }
        // #459: captures by shown frame, for what a paused battle shows over several frames.
        if (next_capture_frame_ < options_.capture_frames.size()
            && shown_frames() >= options_.capture_frames[next_capture_frame_]) {
            if (update.capture_suffix) {
                return fail("capture frame " + std::to_string(options_.capture_frames[next_capture_frame_])
                            + " shows a capture tick too; ask for one of them");
            }
            char suffix[32];
            std::snprintf(suffix, sizeof(suffix), "_f%04llu",
                          static_cast<unsigned long long>(options_.capture_frames[next_capture_frame_]));
            update.capture_suffix = suffix;
            ++next_capture_frame_;
        }
        const std::uint64_t end = std::max<std::uint64_t>(options_.end_tick.value_or(0U),
            options_.capture_ticks.empty() ? 0U : options_.capture_ticks.back());
        update.hold = presented_tick_ + 1.0e-9 < static_cast<double>(end);
        if (battle_end_ && battle_end_->ended_frame) {
            // BEP-02: the battle halted; the run keeps drawing the frames it would have taken to
            // reach its last tick, so the end panel can be looked at and clicked, then stops.
            const double rest = std::max(0.0, static_cast<double>(end) - static_cast<double>(battle_end_->end_tick));
            const auto frames = static_cast<std::uint64_t>(std::ceil(rest / options_.ticks_per_frame - 1.0e-9));
            update.hold = shown_frames() < *battle_end_->ended_frame + frames;
        }
        if (next_capture_frame_ < options_.capture_frames.size()) update.hold = true;
    }
    update.quit = quit_;
    return UpdateResult::success(std::move(update));
}

std::optional<ui::UnitAbilityState> LiveSessionView::Abilities::state(const sim::EntityId unit,
                                                                     const std::uint32_t ability) const {
    const tactical::AbilityKind kind = tactical::ability_kind(ui::ability_name(ability));
    // AB-03: a cut ability the unit's type authors keeps its button, never usable.
    if (kind == tactical::AbilityKind::none) return ui::UnitAbilityState{ui::AbilityStatus::disabled, 1.0, false};
    const auto& snapshot = view_.ability_snapshot_;
    if (!snapshot) return std::nullopt;
    std::vector<sim::EntityId> holders{unit};
    // A squadron's container stands for its craft (AB-15), except for a team ability, which the
    // container holds itself (#561, AB-60).
    const auto squadron = view_.squadron_members_.find(unit);
    if (squadron != view_.squadron_members_.end() && kind != tactical::AbilityKind::ion_cannon_shot) {
        holders = squadron->second;
    }
    bool has = false;
    bool all_active = true;
    bool all_autofire = true;
    bool blocked = false;
    std::optional<double> recharge;
    std::optional<double> active_dial; // AB-05: the largest share of a timed duration left among the units that are on
    for (const tactical::TacticalInstance& instance : snapshot->instances()) {
        if (std::find(holders.begin(), holders.end(), instance.entity_id) == holders.end()) continue;
        for (const tactical::AbilityStatus& status : instance.abilities) {
            if (status.kind != kind) continue;
            has = true;
            all_active = all_active && status.active;
            all_autofire = all_autofire && status.autofire;
            if (status.active && status.remaining_frames > 0 && status.total_frames > 0) {
                const double left = static_cast<double>(status.remaining_frames) / status.total_frames;
                active_dial = active_dial ? std::max(*active_dial, left) : left;
            }
            if (!status.active && !status.ready) {
                if (status.remaining_frames > 0 && status.total_frames > 0) {
                    const double done = 1.0 - static_cast<double>(status.remaining_frames) / status.total_frames;
                    recharge = recharge ? std::min(*recharge, done) : done;
                } else {
                    blocked = true; // AB-14, AB-16: its gate holds it
                }
            }
        }
    }
    if (!has) return std::nullopt;
    ui::UnitAbilityState result;
    result.autofire = all_autofire;
    if (all_active) {
        result.status = ui::AbilityStatus::active;
        if (active_dial) result.recharge = std::clamp(*active_dial, 0.0, 1.0);
    } else if (recharge) {
        result.status = ui::AbilityStatus::recharging;
        result.recharge = std::clamp(*recharge, 0.0, 1.0);
    } else if (blocked) {
        result.status = ui::AbilityStatus::disabled;
    }
    return result;
}

void LiveSessionView::Abilities::request(const ui::AbilityRequest& request) {
    const tactical::AbilityKind kind = tactical::ability_kind(ui::ability_name(request.ability));
    // #561: a targeted activation goes out once it has its target (the battle input's targeting).
    const bool aimed = request.targeted && request.target != sim::invalid_entity_id;
    if (kind == tactical::AbilityKind::none || (request.targeted && !aimed) || !view_.scheduler_) {
        ++refused_;
        return;
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::ability;
    intent.units = request.units;
    intent.unit_ability = kind;
    intent.target = aimed ? request.target : sim::invalid_entity_id;
    intent.origin = aimed ? ui::CommandOrigin::world_click : ui::CommandOrigin::hud_button;
    switch (request.kind) {
    case ui::AbilityRequest::Kind::activate: intent.ability_action = tactical::AbilityAction::activate; break;
    case ui::AbilityRequest::Kind::deactivate: intent.ability_action = tactical::AbilityAction::deactivate; break;
    case ui::AbilityRequest::Kind::autofire_on: intent.ability_action = tactical::AbilityAction::autofire_on; break;
    case ui::AbilityRequest::Kind::autofire_off: intent.ability_action = tactical::AbilityAction::autofire_off; break;
    }
    // The simulation decides which units act; a unit that cannot is rejected alone (AB-11 to AB-15).
    if (view_.scheduler_->issue(intent)) {
        ++issued_;
        if (intent.ability_action == tactical::AbilityAction::activate
            || intent.ability_action == tactical::AbilityAction::deactivate) {
            view_.ability_clicks_.push_back({kind, intent.ability_action == tactical::AbilityAction::activate, request.units});
        }
    } else {
        ++refused_;
    }
}

const tactical::EconomyView* LiveSessionView::local_economy() const noexcept {
    if (!battle_frame_.latest) return nullptr;
    for (const tactical::EconomyView& view : battle_frame_.latest->economy()) {
        if (view.player == player_) return &view;
    }
    return nullptr;
}

std::optional<tactical::FactionId> LiveSessionView::local_faction_id() const noexcept {
    if (!setup_) return std::nullopt;
    for (const tactical::Player& player : setup_->players) {
        if (player.player_id == player_) return player.faction_id;
    }
    return std::nullopt;
}

namespace {

// #530: an economy intent through the order scheduler; false when the scheduler refused it.
bool issue_economy(ui::CommandScheduler* scheduler, const ui::TacticalIntent& intent) {
    return scheduler != nullptr && static_cast<bool>(scheduler->issue(intent));
}

} // namespace

bool LiveSessionView::buy(const sim::EntityId station, const tactical::TypeId type) {
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::buy;
    intent.units = {station};
    intent.type = type;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.buys : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::cancel_build(const tactical::BuildQueue queue, const std::uint32_t index) {
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::cancel;
    intent.queue = queue;
    intent.index = index;
    intent.origin = ui::CommandOrigin::hud_button;
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.cancels : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::reinforce(const tactical::TypeId type, const sim::math::Vec3& point) {
    // WR-15: drop checks placement before room. A busy preview query has no authority;
    // the cached same-point verdict may be used, and the command independently rechecks state.
    const auto valid = session_ ? session_->reinforcement_point(player_, type, point) : std::optional<bool>{false};
    const bool cached = preview_type_ == type && preview_point_ == point && preview_valid_;
    if (!reinforcement_allowed() || !valid.value_or(cached) || !reinforcement_room(type)) {
        ++economy_requests_.refused;
        return false;
    }
    ui::TacticalIntent intent;
    intent.verb = ui::TacticalVerb::reinforce;
    intent.type = type;
    intent.destination = point;
    intent.origin = ui::CommandOrigin::world_click;
    const bool issued = issue_economy(scheduler_.get(), intent);
    ++(issued ? economy_requests_.reinforcements : economy_requests_.refused);
    return issued;
}

bool LiveSessionView::reinforcement_allowed() const noexcept {
    if (!setup_ || outcome_ || local_economy() == nullptr) return false;
    const auto found = std::find_if(setup_->players.begin(), setup_->players.end(), [this](const tactical::Player& player) {
        return player.player_id == player_;
    });
    return found != setup_->players.end() && (found->flags & tactical::player_flag_commandable) != 0;
}

bool LiveSessionView::reinforcement_room(const tactical::TypeId type) const noexcept {
    const auto* ledger = local_economy();
    if (ledger == nullptr || std::find(ledger->pool.begin(), ledger->pool.end(), type) == ledger->pool.end()) return false;
    for (const auto& menu : economy_.menus) {
        if (const auto* option = menu.find(type)) {
            return ledger->population <= ledger->population_cap && option->population <= ledger->population_cap - ledger->population;
        }
    }
    return false;
}

void LiveSessionView::placement_preview(const std::optional<tactical::TypeId> type,
    const std::optional<sim::math::Vec3> point) {
    if (preview_type_ != type || preview_point_ != point) {
        preview_valid_ = false;
        preview_checked_tick_.reset();
    }
    preview_type_ = type;
    preview_point_ = point;
    if (!type || !point || !reinforcement_allowed()) return;
    // WR-13: an unchanged cursor and snapshot need one predicate query, even while paused.
    const auto tick = session_->completed_tick();
    if (preview_checked_tick_ == tick) return;
    ++preview_queries_;
    if (const auto valid = session_->reinforcement_point(player_, *type, *point)) {
        preview_valid_ = *valid;
        preview_checked_tick_ = tick;
    }
}

std::optional<animation::DeathFrame> LiveSessionView::clone_death_frame(const DeathClone& clone,
                                                                       const animation::Player* player,
                                                                       const std::uint64_t tick) {
    if (animation::death_start(player != nullptr, clone.remove_upon_death) == animation::DeathStart::removed) {
        return std::nullopt;
    }
    return animation::death_frame(clone.playback, player ? player->playable_frames() : 0U,
        player ? static_cast<std::uint32_t>(player->frames_per_second()) : tactical::logical_frames_per_second,
        tick, tactical::logical_frames_per_second);
}

std::optional<LiveSessionView::CloneFrame> LiveSessionView::clone_frame(const SpacePopulation& population,
                                                                        const std::size_t ship,
                                                                        const double tick) const {
    // A clone that left this frame still stood in the samples before its fade (#429).
    for (const std::vector<ActiveClone>* clones : {&active_clones_, &retiring_clones_}) {
        for (const ActiveClone& active : *clones) {
            const DeathClone& clone = death_clones_.at(active.unit);
            if (clone.ship != ship) continue;
            // frame()'s clock: whole ticks since the clone appeared, one tick before its death tick.
            const double elapsed = tick - (static_cast<double>(active.death_tick) - 1.0);
            if (elapsed < -1.0e-9) return std::nullopt;
            const animation::Player* player = population.live_clip(ship);
            const auto death = clone_death_frame(clone, player,
                                                 static_cast<std::uint64_t>(std::floor(elapsed + 1.0e-9)));
            if (!death || !death->shown) return std::nullopt;
            CloneFrame result{active.pose, std::nullopt};
            if (player != nullptr) result.death = *death;
            return result;
        }
    }
    return std::nullopt;
}

void LiveSessionView::retire_clones(SpacePopulation& population, GodotRenderer& renderer) {
    for (const ActiveClone& retiring : retiring_clones_) {
        population.retire_live_ship(renderer, death_clones_.at(retiring.unit).ship);
    }
    retiring_clones_.clear();
}

void LiveSessionView::finish() {
    if (finished_ || !session_) return;
    finished_ = true;
    shutdown_trace::mark("finish: session stop begins");
    session_->stop();
    shutdown_trace::mark("finish: session stopped");
    hashes_ = session_->tick_hashes();
    const bool verify = options_.verify.value_or(!options_.real_time);
    tactical::TacticalReplay replay;
    if (verify || !options_.replay_path.empty()) replay = session_->record();
    shutdown_trace::mark("finish: " + std::to_string(hashes_.size()) + " ticks, replay verification "
                         + (verify ? "on" : "off"));
    finish_status_ = "stopped";
    if (verify) {
        // The viewer-attached run against a headless run of the same command stream.
        auto headless = platform::headless_tick_hashes(replay, content_->sensors, content_->durability, content_->motion,
                                                       content_->combat, victory_, content_->fog, content_->abilities,
                                                       economy_);
        shutdown_trace::mark("finish: headless replay done");
        headless_equal_ = headless && headless.value() == hashes_;
        if (!headless) finish_status_ = "headless replay failed: " + core::format_diagnostic(headless.error());
    }
    if (const auto failure = session_->failure()) {
        simulation_error_ = core::format_diagnostic(*failure);
        finish_status_ = "simulation_failed";
        save_failure_replay();
    }
    if (!options_.hashes_path.empty()) {
        std::ofstream output(options_.hashes_path, std::ios::binary | std::ios::trunc);
        output << "tick,sha256\n";
        for (std::size_t index = 0; index < hashes_.size(); ++index) output << index + 1 << ',' << hashes_[index] << '\n';
        if (!output) finish_status_ = "could not write " + ViewerPath::utf8(options_.hashes_path);
    }
    if (!options_.replay_path.empty()) {
        auto bytes = tactical::write_replay(replay);
        std::ofstream output(options_.replay_path, std::ios::binary | std::ios::trunc);
        if (bytes) output.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
        if (!bytes || !output) finish_status_ = "could not write " + ViewerPath::utf8(options_.replay_path);
        // #459 TP-04: the time track beside the replay; the replay itself is unchanged.
        std::filesystem::path time_path = options_.replay_path;
        time_path += ".time.csv";
        std::ofstream time(time_path, std::ios::binary | std::ios::trunc);
        time << time_.track_csv();
        if (!time) finish_status_ = "could not write " + ViewerPath::utf8(time_path);
    }
}

void LiveSessionView::save_failure_replay() {
    if (failure_replay_tried_ || !session_ || !session_->failure()) return;
    failure_replay_tried_ = true;
    const tactical::TacticalReplay replay = session_->failure_record();
    auto bytes = tactical::write_replay(replay);
    if (!bytes) {
        failure_replay_error_ = core::format_diagnostic(bytes.error());
        return;
    }
    // Beside godot.log: Godot's file log setting, user://logs/godot.log by default.
    auto* settings = godot::ProjectSettings::get_singleton();
    const godot::String log = settings->get_setting("debug/file_logging/log_path", godot::String("user://logs/godot.log"));
    const godot::CharString global = settings->globalize_path(log).utf8();
    const std::filesystem::path directory =
        ViewerPath{std::string(global.get_data(), static_cast<std::size_t>(global.length()))}.native().parent_path();
    const auto seconds = static_cast<std::int64_t>(godot::Time::get_singleton()->get_unix_time_from_system());
    // Concurrent lanes may fail at the same second and tick. Include the process
    // and monotonic time so even a shared custom log directory keeps both replays.
    const auto process = godot::OS::get_singleton()->get_process_id();
    const auto usec = godot::Time::get_singleton()->get_ticks_usec();
    const std::filesystem::path path = directory
        / ("eawr-live-failure-" + std::to_string(seconds) + "-pid" + std::to_string(process)
           + "-usec" + std::to_string(usec) + "-tick" + std::to_string(replay.final_tick_count) + ".eawr-replay");
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
    output.close();
    if (!output) {
        failure_replay_error_ = "could not write " + ViewerPath::utf8(path);
        godot::UtilityFunctions::printerr(godot::String(("live session: " + failure_replay_error_).c_str()));
        return;
    }
    failure_replay_ = ViewerPath::utf8(path);
    godot::UtilityFunctions::print(godot::String(("live session: the failure replay is " + failure_replay_).c_str()));
}

void LiveSessionView::attach_debris(DebrisProps& debris) {
    if (!tables_ || !content_) return;
    // The session's units are the first placed ships.
    debris.prepare(*tables_, content_->combat, placed_ships_, ship_of_entity_.size());
    debris_ = &debris;
}

void LiveSessionView::attach_projectile_models(BattleEffects& effects) {
    effects.plan_projectile_models(placed_ships_);
    projectile_models_ = &effects;
}

std::optional<BattleEffects::UnitFrame> LiveSessionView::unit_frame(const sim::EntityId entity) const {
    const auto found = unit_frames_.find(entity);
    return found == unit_frames_.end() ? std::nullopt : std::optional(found->second);
}

const tactical::Squadron* LiveSessionView::squadron_of(const sim::EntityId craft) const noexcept {
    const auto container = squadron_of_.find(craft);
    if (container == squadron_of_.end()) return nullptr;
    const auto squadron = squadrons_.find(container->second);
    return squadron == squadrons_.end() ? nullptr : &squadron->second;
}

std::vector<platform::LiveTickCost> LiveSessionView::tick_costs_after(const std::uint64_t after) const {
    return session_ ? session_->tick_costs_after(after) : std::vector<platform::LiveTickCost>{};
}

std::shared_ptr<const tactical::TacticalSnapshot> LiveSessionView::snapshot_at(const std::uint64_t tick) const {
    return session_ ? session_->snapshot_at(tick) : nullptr;
}

void LiveSessionView::write_report(std::ostream& output) const {
    output << "  \"live_session\": {\"fixture\": " << json(options_.fixture)
           << ", \"replay\": " << json(ViewerPath::utf8(options_.replay_input))
           << ", \"reveal\": " << (options_.reveal ? "true" : "false")
           << ", \"pacing\": " << json(options_.real_time ? "real_time" : "driven")
           << ", \"workers\": " << (session_ ? session_->worker_count() : 0U)
           << ", \"local_player\": " << player_
           << ", \"units\": " << ship_of_entity_.size()
           << ", \"launch_slots\": " << launch_slots_.size()
           << ", \"launched_drawn\": " << launched_ship_of_entity_.size()
           << ", \"slots_released\": " << slots_released_
           << ", \"squadrons\": [" << [&] {
                  // #518: every registered squadron, the setup's and the launched, as first seen.
                  std::string rows;
                  for (const auto& [container, squadron] : squadrons_) {
                      std::string members;
                      for (const sim::EntityId member : squadron.members) {
                          members += (members.empty() ? "" : ", ") + std::to_string(member);
                      }
                      // The owner from the newest snapshot that still holds the container, else 0.
                      tactical::PlayerId owner{};
                      if (battle_frame_.latest) {
                          for (const tactical::TacticalInstance& instance : battle_frame_.latest->instances()) {
                              if (instance.entity_id == container) owner = instance.owner;
                          }
                      }
                      const bool launched = !setup_ || std::none_of(setup_->squadrons.begin(), setup_->squadrons.end(),
                          [&](const tactical::Squadron& start) { return start.container == container; });
                      // #614: how many of its craft have an S-foil clip pair, and how many hold the foils locked.
                      std::size_t foil_craft = 0;
                      std::size_t foils_locked = 0;
                      for (const sim::EntityId member : squadron.members) {
                          if (const auto foil = sfoils_.find(member); foil != sfoils_.end()) {
                              ++foil_craft;
                              foils_locked += foil->second.on ? 1U : 0U;
                          }
                      }
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"container\": ") + std::to_string(container)
                          + ", \"sfoil_craft\": " + std::to_string(foil_craft)
                          + ", \"sfoils_locked\": " + std::to_string(foils_locked)
                          + ", \"owner\": " + std::to_string(owner) + ", \"launched\": " + (launched ? "true" : "false")
                          + ", \"seen_tick\": " + std::to_string(squadron_seen_.at(container))
                          + ", \"members\": [" + members + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"session_records\": " << session_records_.size()
           << ", \"frames\": " << frames_
           << ", \"presented_tick\": " << presented_tick_
           << ", \"latest_tick\": " << latest_tick_
           << ", \"stall\": " << (options_.stall ? "{\"tick\": "
                                                     + (options_.stall->start ? std::string("\"start\"")
                                                                              : std::to_string(options_.stall->tick))
                                                     + ", \"ticks\": " + std::to_string(options_.stall->ticks) + "}"
                                                 : std::string("null"))
           << ", \"visible_units\": " << visible_units_
           << ", \"hidden_units\": " << hidden_units_
           << ", \"fading_units\": " << fading_units_
           << ", \"fading\": ["
           << [&] {
                  std::string rows;
                  for (const sim::EntityId entity : fade_.drawn()) {
                      const auto opacity = fade_.opacity(entity);
                      if (!opacity) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"entity\": ") + std::to_string(entity)
                          + ", \"opacity\": " + std::to_string(*opacity) + "}";
                  }
                  return rows;
              }()
           << "]"
           << ", \"fading_log\": [" << [&] {
                  std::string rows;
                  for (const std::string& row : fading_log_rows_) rows += (rows.empty() ? "" : ", ") + row;
                  return rows;
              }() << "]"
           << ", \"scripted_inputs\": " << options_.inputs.size()
           << ", \"start_map\": " << json(start_ ? start_->map : "")
           << ", \"start_map_sha256\": " << json(start_ ? start_->map_sha256 : "")
           << ", \"start_seed\": " << (setup_ ? setup_->seed : 0)
           << ", \"start_markers\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& marker : start_->markers) {
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"record\": ") + std::to_string(marker.record)
                          + ", \"player\": " + std::to_string(marker.player) + ", \"use\": " + json(skirmish::to_string(marker.use))
                          + ", \"position\": [" + std::to_string(to_float(marker.position.x)) + ", "
                          + std::to_string(to_float(marker.position.y)) + ", " + std::to_string(to_float(marker.position.z)) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"start_fleet\": [" << [&] {
                  std::string rows;
                  if (start_) for (const auto& unit : start_->units) {
                      if (unit.role != skirmish::UnitRole::fleet && unit.role != skirmish::UnitRole::free_unit) continue;
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"entity\": ") + std::to_string(unit.state.entity_id)
                          + ", \"player\": " + std::to_string(unit.state.owner) + ", \"record\": " + std::to_string(unit.record)
                          + ", \"type\": " + json(unit.type) + ", \"position\": [" + std::to_string(to_float(unit.state.position.x))
                          + ", " + std::to_string(to_float(unit.state.position.y)) + ", "
                          + std::to_string(to_float(unit.state.position.z)) + "]}";
                  }
                  return rows;
              }() << "]"
           << ", \"ai_flag_ignored\": " << (ai_flag_ignored_ ? "true" : "false")
           << ", \"time\": {\"speed_step\": " << time_.speed_step() << ", \"state\": "
           << json(time_.ended() ? std::string("ended") : std::string(ui::to_string(time_.state())))
           << ", \"target_rate\": " << time_.target_rate() << ", \"track\": ["
           << [&] {
                  std::string rows;
                  for (const ui::TimeChange& change : time_.track()) {
                      rows += (rows.empty() ? "" : ", ") + std::string("{\"tick\": ") + std::to_string(change.tick)
                          + ", \"state\": " + json(ui::to_string(change.state)) + ", \"ticks_per_second\": "
                          + std::to_string(change.target_rate) + ", \"cause\": " + json(change.cause) + "}";
                  }
                  return rows;
              }()
           << "]}"
           << ", \"defend\": " << (options_.defend ? "true" : "false")
           << ", \"shield_flash\": " << (options_.shield_flash ? "true" : "false")
           << ", \"shield_flashes\": " << shield_flashes_
           << ", \"own_units\": [";
    bool first_unit = true;
    for (const VisibleUnit& unit : visible_) {
        if (!unit.own) continue;
        output << (first_unit ? "" : ", ") << "{\"entity\": " << unit.entity << ", \"position\": [" << unit.position[0]
               << ", " << unit.position[1] << ", " << unit.position[2] << "], \"yaw\": " << unit.yaw << "}";
        first_unit = false;
    }
    output << "]"
           << ", \"hostile_units\": [";
    bool first_hostile = true;
    for (const VisibleUnit& unit : visible_) {
        if (!unit.hostile) continue;
        output << (first_hostile ? "" : ", ") << "{\"entity\": " << unit.entity << "}";
        first_hostile = false;
    }
    output << "]"
           << ", \"orders\": [";
    for (std::size_t index = 0; index < options_.orders.size(); ++index) {
        const ScheduledOrder& order = options_.orders[index];
        output << (index ? ", " : "") << "{\"tick\": " << order.tick << ", \"kind\": " << json(tactical::to_string(order.kind))
               << ", \"unit\": " << order.unit;
        if (order.target != 0) output << ", \"target\": " << order.target;
        output << "}";
    }
    output << "], \"first_hits\": [";
    for (auto hit = first_hits_.begin(); hit != first_hits_.end(); ++hit) {
        output << (hit == first_hits_.begin() ? "" : ", ") << "{\"shooter\": " << hit->first.first
               << ", \"target\": " << hit->first.second << ", \"tick\": " << hit->second << "}";
    }
    output << "], \"rejected\": [";
    if (session_) {
        const auto rejected = session_->rejected_orders();
        for (std::size_t index = 0; index < rejected.size(); ++index) {
            output << (index ? ", " : "") << json(rejected[index].code + " " + rejected[index].message);
        }
    }
    output << "], \"ai\": ";
    if (session_ && ai_scripts_) {
        // #79: the FoC AI's players, Lua load and the calls it could not make.
        const auto report = session_->script_report();
        output << "{\"players\": [";
        for (std::size_t index = 0; index < ai_players_.size(); ++index) output << (index ? ", " : "") << ai_players_[index];
        output << "], \"ticks\": " << report.ticks << ", \"max_instructions\": " << report.max_instructions
               << ", \"total_instructions\": " << report.total_instructions << ", \"diagnostics\": [";
        for (std::size_t index = 0; index < report.diagnostics.size(); ++index) {
            output << (index ? ", " : "") << json(report.diagnostics[index]);
        }
        output << "]}";
    } else {
        output << "null";
    }
    output << ", \"unit_clips\": [";
    for (std::size_t index = 0; index < unit_clip_rows_.size(); ++index) output << (index ? ", " : "") << unit_clip_rows_[index];
    output << "], \"ion_shots\": {\"squadrons\": [";
    for (auto row = ion_shot_rows_.begin(); row != ion_shot_rows_.end(); ++row) {
        output << (row == ion_shot_rows_.begin() ? "" : ", ") << "{\"squadron\": " << row->first << ", \"switched_on\": "
               << row->second.switched_on << ", \"first_on\": " << row->second.first_on << ", \"last_on\": "
               << row->second.last_on << ", \"on\": " << (row->second.on ? "true" : "false") << "}";
    }
    output << "], \"stunned\": [";
    for (auto row = ion_stun_rows_.begin(); row != ion_stun_rows_.end(); ++row) {
        output << (row == ion_stun_rows_.begin() ? "" : ", ") << "{\"unit\": " << row->first << ", \"first\": "
               << row->second.first << ", \"max_frames\": " << row->second.max_frames << "}";
    }
    output << "]}, \"sfoil_switches\": " << sfoil_switches_ << ", \"ability_requests\": {\"issued\": "
           << abilities_.issued() << ", \"refused\": " << abilities_.refused() << "}";
    // #530: the economy requests, the local player's economy at the last frame and the arrivals.
    output << ", \"economy_requests\": {\"buys\": " << economy_requests_.buys << ", \"cancels\": "
           << economy_requests_.cancels << ", \"reinforcements\": " << economy_requests_.reinforcements
           << ", \"refused\": " << economy_requests_.refused << "}, \"economy\": ";
    if (const tactical::EconomyView* view = local_economy()) {
        output << "{\"credits\": " << json(ui::credits_text(view->credits)) << ", \"population\": " << view->population
               << ", \"population_cap\": " << view->population_cap << ", \"queued\": ["
               << view->queues[0].size() << ", " << view->queues[1].size() << "], \"pool\": [";
        for (std::size_t index = 0; index < view->pool.size(); ++index) output << (index ? ", " : "") << view->pool[index];
        output << "]}";
    } else {
        output << "null";
    }
    output << ", \"placement_preview\": {\"frames\": " << preview_frames_ << ", \"queries\": "
           << preview_queries_ << ", \"samples\": [";
    for (std::size_t index = 0; index < preview_rows_.size(); ++index) output << (index ? ", " : "") << preview_rows_[index];
    output << "]}, \"arrivals\": [";
    std::size_t arrival_index = 0;
    for (const auto& [entity, row] : arrivals_) {
        output << (arrival_index++ ? ", " : "") << "{\"unit\": " << entity << ", \"owner\": " << row.owner
               << ", \"type\": " << row.type << ", \"first_tick\": " << row.first_tick << ", \"visible_tick\": "
               << row.visible_tick << ", \"landed_tick\": " << row.landed_tick << "}";
    }
    output << "], \"death_clones\": [";
    for (std::size_t index = 0; index < death_clone_rows_.size(); ++index) output << (index ? ", " : "") << death_clone_rows_[index];
    output << "], \"death_clones_shown\": [";
    for (std::size_t index = 0; index < active_clones_.size(); ++index) {
        output << (index ? ", " : "") << "{\"unit\": " << active_clones_[index].unit
               << ", \"death_tick\": " << active_clones_[index].death_tick << "}";
    }
    output << "], \"death_clones_retired\": [";
    for (std::size_t index = 0; index < retired_clone_rows_.size(); ++index) {
        output << (index ? ", " : "") << retired_clone_rows_[index];
    }
    output << "], \"spin_away\": {\"drawn_max\": " << spinning_drawn_max_ << ", \"ships_max\": " << spinning_ships_max_
           << ", \"spins\": [";
    bool first_spin = true;
    for (const auto& [unit, row] : spin_rows_) {
        output << (first_spin ? "" : ", ") << "{\"unit\": " << unit << ", \"started\": " << row.started
               << ", \"ended\": " << (row.ended ? std::to_string(*row.ended) : std::string("null")) << "}";
        first_spin = false;
    }
    output << "]}, \"event_gaps\": [";
    for (std::size_t index = 0; index < event_gap_rows_.size(); ++index) output << (index ? ", " : "") << event_gap_rows_[index];
    output << "], \"completed_ticks\": " << hashes_.size()
           << ", \"final_state_sha256\": " << json(hashes_.empty() ? "" : hashes_.back())
           << ", \"headless_hashes_equal\": " << (headless_equal_ ? (*headless_equal_ ? "true" : "false") : "null")
           << ", \"outcome\": ";
    if (outcome_) {
        // The local player's result: the winner's team won, every other lobby team lost.
        const auto team = team_of_player_.find(player_);
        const bool won = team != team_of_player_.end() && team->second == outcome_->winner_team;
        output << "{\"condition\": " << json(tactical::to_string(outcome_->condition)) << ", \"winner\": "
               << outcome_->winner << ", \"winner_team\": " << outcome_->winner_team << ", \"decided_tick\": "
               << outcome_->decided_tick << ", \"deciding_unit\": " << outcome_->deciding_unit
               << ", \"end_tick\": " << outcome_->end_tick << ", \"local_result\": " << json(won ? "victory" : "defeat")
               << "}";
    } else {
        output << "null";
    }
    output << ", \"battle_end\": ";
    if (battle_end_) {
        output << "{\"message\": " << json(ui::battle_message_key(battle_end_->result))
               << ", \"title\": " << json(ui::battle_end_title_key(battle_end_->result))
               << ", \"decided_tick\": " << battle_end_->decided_tick << ", \"end_tick\": " << battle_end_->end_tick
               << ", \"shown_frame\": " << battle_end_->shown_frame << ", \"ended_frame\": "
               << (battle_end_->ended_frame ? std::to_string(*battle_end_->ended_frame) : std::string("null"))
               << ", \"halt_tick\": "
               << (session_ && session_->halt_tick() ? std::to_string(*session_->halt_tick()) : std::string("null"))
               << ", \"quit\": " << (quit_ ? "true" : "false") << "}";
    } else {
        output << "null";
    }
    output << ", \"status\": " << json(finish_status_)
           << ", \"error\": " << (simulation_error_.empty() ? std::string("null") : json(simulation_error_))
           << ", \"failure_replay\": " << (failure_replay_.empty() ? std::string("null") : json(failure_replay_)) << "},\n";
}

std::string LiveSessionView::player_faction(const tactical::PlayerId player) const {
    if (!start_) return {};
    for (const skirmish::StartPlayer& candidate : start_->players) {
        if (candidate.player.player_id == player) return candidate.faction;
    }
    return {};
}

std::optional<tactical::TeamId> LiveSessionView::team_of(const tactical::PlayerId player) const {
    const auto found = team_of_player_.find(player);
    if (found == team_of_player_.end()) return std::nullopt;
    return found->second;
}

std::string LiveSessionView::local_faction() const {
    if (start_) {
        for (const skirmish::StartPlayer& player : start_->players) {
            if (player.player.player_id == player_) return player.faction;
        }
        return {};
    }
    // A replay names factions by ID only (#84: the music lists): the playable factions' names.
    if (!setup_) return {};
    for (const tactical::Player& player : setup_->players) {
        if (player.player_id != player_) continue;
        for (const std::string_view name : {"Empire", "Rebel", "Underworld"}) {
            if (skirmish::faction_id(name) == player.faction_id) return std::string(name);
        }
    }
    return {};
}

} // namespace eawr::presentation::godot_backend
