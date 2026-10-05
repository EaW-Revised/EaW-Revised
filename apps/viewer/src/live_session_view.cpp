#include "eawr/core/load_profile.hpp"
#include "live_session_view.hpp"
#include "live_session_internal.hpp"
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

namespace eawr::presentation::godot_backend {
namespace tactical = sim::tactical;
using namespace live_session_detail;
namespace live_session_detail {
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

[[nodiscard]] float to_float(const sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
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

} // namespace live_session_detail

namespace {

// A Death_* seconds tag of the clone type; `fallback` when absent or not a number.
} // namespace

LiveSessionView::LiveSessionView(Options options) : options_(std::move(options)), time_(options_.speed_step) {}

LiveSessionView::~LiveSessionView() {
    shutdown_trace::mark("live session view: stop begins");
    if (session_) session_->stop();
    shutdown_trace::mark("live session view: stopped");
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

} // namespace eawr::presentation::godot_backend
