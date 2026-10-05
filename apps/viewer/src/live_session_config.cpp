#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "eawr/presentation/ui/pads.hpp"

#include "shutdown_trace.hpp"
#include "frame_timer.hpp"

#include "eawr/platform/live_ai.hpp"
#include "eawr/presentation/space/live_units.hpp"
#include "eawr/presentation/space/unit_fade.hpp"
#include "eawr/presentation/ui/production.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/roster_gate.hpp"
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

#include "live_session_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;

namespace live_session_detail {
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
} // namespace live_session_detail

namespace {

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
            if (order.ability == tactical::AbilityKind::weaken_enemy) {
                std::vector<sim::math::Fixed> values;
                std::size_t start = aim + 1;
                for (;;) {
                    const auto next = action.find(',', start);
                    const auto value = parse_double(action.substr(start, next == std::string::npos ? std::string::npos : next - start));
                    if (!value) return std::nullopt;
                    auto fixed = scene::fixed_from_binary32(static_cast<float>(*value));
                    if (!fixed) return std::nullopt;
                    values.push_back(fixed.value());
                    if (next == std::string::npos) break;
                    start = next + 1;
                }
                if (values.size() != 3) return std::nullopt;
                order.point = {values[0], values[1], values[2]};
            } else {
            const auto target = parse_u64(std::string_view(action).substr(aim + 1));
            if (!target || *target == 0) return std::nullopt;
            order.target = *target;
            }
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
        if (input.kind != "click" && input.kind != "rclick" && input.kind != "hover"
            && input.kind != "press" && input.kind != "release") return std::nullopt;
        const auto digits = [](std::string_view text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        };
        const std::string_view hud = input.hud;
        const bool time = hud == "begin" || hud == "pause" || hud == "fast_forward" || hud == "resume" || hud == "quit";
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
    if (input.kind != "click" && input.kind != "dclick" && input.kind != "rclick" && input.kind != "rdclick" && input.kind != "hover"
        && input.kind != "press" && input.kind != "release") {
        return std::nullopt;
    }
    if (target.rfind("icon=", 0) == 0 && (input.kind == "hover" || input.kind == "click" || input.kind == "dclick"
        || input.kind == "rclick" || input.kind == "rdclick")) {
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
    } else if (name == "--eawr-skirmish-victory") {
        if (text == "starbase") options.skirmish.victory_condition = tactical::VictoryCondition::enemy_starbase_destroyed;
        else if (text == "all-units") options.skirmish.victory_condition = tactical::VictoryCondition::all_enemy_units_destroyed;
        else error = "--eawr-skirmish-victory expects starbase or all-units";
    } else if (name == "--eawr-skirmish-seed") {
        const auto seed = parse_u64(text);
        if (!seed) error = "--eawr-skirmish-seed expects an unsigned whole number";
        else options.skirmish.seed = *seed;
    } else if (name == "--eawr-skirmish-players") {
        std::vector<skirmish::LobbySlot> slots;
        std::size_t begin = 0;
        std::uint64_t previous{};
        while (begin < text.size()) {
            const auto end = text.find(',', begin);
            const auto id = parse_u64(text.substr(begin, end == std::string::npos ? end : end - begin));
            if (!id || *id <= previous || *id > tactical::max_players || slots.size() >= 8) break;
            skirmish::LobbySlot slot = slots.empty() ? skirmish::m2_fixture().slots[0] : skirmish::m2_fixture().slots[1];
            slot.slot = static_cast<std::uint32_t>(*id);
            slots.push_back(std::move(slot));
            previous = *id;
            if (end == std::string::npos) { begin = text.size(); break; }
            begin = end + 1;
            if (begin == text.size()) { begin = text.size() + 1; break; }
        }
        if (begin != text.size() || slots.size() < 2)
            error = "--eawr-skirmish-players requires two to eight increasing slot IDs";
        else options.skirmish.slots = std::move(slots);
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
                error = "slot is not in the selected skirmish roster; set --eawr-skirmish-players first";
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
                    error = "--eawr-skirmish-slot expects <slot>:<faction>:<team>:<human|ai>";
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
    } else if (name == "--eawr-live-begin") {
        if (text != "manual" && text != "auto") error = "--eawr-live-begin expects manual or auto";
        else options.begin_barrier = text == "manual";
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
                    "or <tick>:wheel:out, or <tick>:<click|rclick|hover|press|release>:hud=<pause|fast_forward|resume|quit|"
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
    } else if (name == "--eawr-live-particle-detail" || name == "--eawr-live-particle-lod") {
        const auto value = parse_double(text);
        if (!value || *value < 0 || *value > 1) error = std::string(name) + " expects a value from 0 to 1";
        else if (name == "--eawr-live-particle-detail") options.particle_detail.global = static_cast<float>(*value);
        else options.particle_detail.local = static_cast<float>(*value);
    } else if (name == "--eawr-live-particle-heat") {
        if (text != "on" && text != "off") error = "--eawr-live-particle-heat expects on or off";
        else options.particle_detail.heat = text == "on";
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
    } else if (name == "--eawr-live-follow-projectile") {
        const auto unit = parse_u64(text);
        if (!unit || *unit == 0) error = "--eawr-live-follow-projectile expects a shooter unit ID";
        else options.follow_projectile = static_cast<sim::EntityId>(*unit);
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

} // namespace eawr::presentation::godot_backend
