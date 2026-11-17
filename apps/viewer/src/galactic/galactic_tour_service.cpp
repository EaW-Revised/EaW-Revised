#include "galactic_mode_internal.hpp"

#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/classes/viewport.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <sstream>

namespace eawr::presentation::godot_backend {
using namespace godot;

// #6291 (docs/behaviour/walks/gc-cinematic-tour.md, GCT-01..GCT-12): the galactic tour, the player's cinematic camera on the
// galaxy map. The camera path is pure maths (galactic_tour.hpp); this is the host side: when it may start, the bands and
// GUI hiding through the story HUD state, the camera it hands the frame, and what ends it. Presentation only: it reads the
// planets the view already holds and writes nothing the simulation, a save or a replay sees.

bool GalacticMode::State::tour_allowed() const {
    if (!interactive || !galaxy || !failure.empty() || completed) return false;
    galactic_view::TourGates gates;
    gates.map_showing = !tactical && !loading;
    gates.paused = paused;
    gates.prompt_up = galaxy_modal_open() || (planet_search && planet_search->visible());
    return galactic_view::tour_may_play(gates);
}

bool GalacticMode::State::start_tour() {
    if (!tour_allowed()) return false;
    std::vector<galactic_view::TourPlanet> planets;
    planets.reserve(galaxy->planets.size());
    for (const auto& planet : galaxy->planets)
        planets.push_back({planet.id, planet.position, galactic_view::skeleton_planet_radius * planet.scale * std::numbers::sqrt3_v<float>});
    const auto& box = galaxy->bounds;
    const float hx = (box.max.x - box.min.x) * 0.5F, hy = (box.max.y - box.min.y) * 0.5F, hz = (box.max.z - box.min.z) * 0.5F;
    const float extent = std::sqrt(hx * hx + hy * hy + hz * hz);
    const auto clock = static_cast<std::uint32_t>(Time::get_singleton()->get_ticks_usec());
    galactic_view::TourRandom random(clock ^ static_cast<std::uint32_t>(tours_started * 2654435761ULL));
    std::optional<galactic_view::ObjectId> selected;
    if (selected_planet != 0) selected = selected_planet;
    auto next = galactic_view::GalacticTour::begin(std::move(planets), selected, extent, random);
    if (!next) return false;
    if (tour) ++tour_restarts;
    tour = std::move(next);
    tour_seconds = 0.0;
    ++tours_started;
    story_hud.set_tour(true);
    // The tour owns the camera: a held pan key or drag ends.
    held.clear();
    orbiting = scrolling = false;
    scroll_hold.end();
    map_input.held_scroll = map_input.held_rotate = false;
    apply_camera();
    return true;
}

void GalacticMode::State::end_tour() {
    if (!tour) return;
    tour.reset();
    story_hud.set_tour(false);
    apply_camera(); // back to the camera the player left
}

void GalacticMode::State::advance_tour(const double delta) {
    if (!tour) return;
    if (!tour_allowed()) { end_tour(); return; }
    tour_seconds += std::max(0.0, delta); // a pause or a prompt ended the tour above (GCT-01)
    if (tour_seconds >= tour->duration()) start_tour(); // the next tour; none to start ends this one
    if (tour && tour_seconds >= tour->duration()) end_tour();
}

galactic_view::OrbitCamera GalacticMode::State::tour_camera() const {
    const auto pose = tour->pose(tour_seconds);
    const float dx = pose.eye.x - pose.target.x, dy = pose.eye.y - pose.target.y, dz = pose.eye.z - pose.target.z;
    const float distance = std::max(1.0F, std::sqrt(dx * dx + dy * dy + dz * dz));
    constexpr float degrees = 180.0F / std::numbers::pi_v<float>;
    galactic_view::OrbitCamera camera;
    camera.target = pose.target;
    camera.distance = distance;
    camera.pitch_degrees = std::asin(std::clamp(dz / distance, -1.0F, 1.0F)) * degrees;
    camera.yaw_degrees = std::atan2(-dx, -dy) * degrees; // fixed_camera: yaw 0 looks along +Y from behind and above the target
    return camera;
}

bool GalacticMode::State::tour_input(const Ref<InputEvent>& event) {
    if (!tour) return false;
    // GCT-08: a pointer move smaller than a twentieth of the screen in each axis is ignored (and nothing else sees it); a
    // larger one, a button press and the wheel end the tour, and the event that ends it is not passed on.
    if (const auto* motion = Object::cast_to<InputEventMouseMotion>(event.ptr())) {
        const Vector2 relative = motion->get_relative();
        const Vector2 size = host->get_viewport()->get_visible_rect().size;
        if (std::abs(relative.x) < size.x / 20.0F && std::abs(relative.y) < size.y / 20.0F) return true;
        end_tour();
        return true;
    }
    if (const auto* button = Object::cast_to<InputEventMouseButton>(event.ptr()); button && button->is_pressed()) {
        end_tour();
        return true;
    }
    return false;
}

std::string GalacticMode::State::tour_report_json() const {
    std::ostringstream out;
    out << "{\"active\": " << (tour ? "true" : "false") << ", \"started\": " << tours_started << ", \"restarts\": " << tour_restarts
        << ", \"seconds\": " << tour_seconds << ", \"duration\": " << (tour ? tour->duration() : 0.0)
        << ", \"visiting\": " << (tour ? tour->visiting(tour_seconds) : galactic_view::ObjectId{0})
        << ", \"planets\": " << (tour ? tour->visited().size() : 0U)
        << ", \"letterbox\": " << story_hud.letterbox_progress()
        << ", \"hud_visible\": " << (hud && hud->is_visible() ? "true" : "false") << '}';
    return out.str();
}

} // namespace eawr::presentation::godot_backend
