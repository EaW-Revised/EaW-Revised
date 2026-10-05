#include "battle_input.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"
#include "eawr/skirmish/roster_gate.hpp"
#include "eawr/sim/tactical/types.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world2d.hpp>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <limits>
#include <map>
#include <utility>

namespace eawr::presentation::godot_backend {
using namespace godot;

bool BattleInput::build_click(const std::size_t slot, LiveSessionView& live) {
    refresh_cards(live);
    ++build_clicks_;
    if (pad_palette_.entity()) {
        const auto request = pad_palette_.click(slot, build_buttons_);
        selection_.replace({});
        cards_snapshot_.reset();
        refresh_cards(live);
        const bool issued = request && live.buy(request->first, request->second);
        if (issued) ++buys_;
        note("pad build " + std::to_string(slot) + (issued ? ": dispatched" : ": disabled"));
        return issued;
    }
    if (!production_station_) return false;
    const auto found = std::find_if(build_buttons_.begin(), build_buttons_.end(),
        [slot](const ui::BuildButton& button) { return button.slot == slot; });
    if (found == build_buttons_.end() || !found->enabled) {
        note("build " + std::to_string(slot) + ": disabled");
        return false;
    }
    const bool issued = live.buy(*production_station_, found->type);
    if (issued) ++buys_;
    note("build " + std::to_string(slot) + ": " + (issued ? "buy " : "refused ") + std::to_string(found->type));
    return issued;
}

void BattleInput::begin_placement(const sim::tactical::TypeId type) {
    if (placing_) return; // WR-11: only one active drag
    placing_ = type;
    note("reinforce placing " + std::to_string(type));
}

std::optional<sim::math::Vec3> BattleInput::placement_point() const {
    if (!placing_) return std::nullopt;
    if (!hover_point_) return std::nullopt;
    const auto x = scene::fixed_from_binary32((*hover_point_)[0]);
    const auto y = scene::fixed_from_binary32((*hover_point_)[1]);
    if (!x || !y) return std::nullopt;
    return sim::math::Vec3{x.value(), y.value(), {}};
}

} // namespace eawr::presentation::godot_backend
