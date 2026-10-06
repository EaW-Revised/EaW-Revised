#include "camera_input_internal.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <initializer_list>
#include <set>
#include <tuple>

namespace eawr::viewer::camera_input {
namespace camera_input_detail {

[[nodiscard]] core::Diagnostic failure(const std::string_view code, std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(code);
    diagnostic.severity = core::Severity::error;
    diagnostic.message = std::move(message);
    return diagnostic;
}

[[nodiscard]] bool finite(const float value) noexcept { return std::isfinite(value); }

} // namespace camera_input_detail

// --- Adapter --------------------------------------------------------------------

Adapter::Adapter(const Context context, const bool focused) noexcept
    : context_(context), reducer_(focused) {}

bool Adapter::eligible() const noexcept {
    return table_.has_value() && reducer_.input_eligible() && viewport_known_
        && viewport_width_ > 0.0F && viewport_height_ > 0.0F;
}

void Adapter::cancel() noexcept {
    reducer_.clear();
    pressed_.clear();
    pointer_valid_ = false;
    clear_pending();
    ++counters_.cancellations;
}

void Adapter::clear_pending() noexcept {
    zoom_detents_ = 0.0F;
    rotate_units_ = 0.0F;
    orbit_pitch_units_ = 0.0F;
    translate_x_ = 0.0F;
    translate_y_ = 0.0F;
    pan_motion_x_ = 0.0F;
    pan_motion_y_ = 0.0F;
    grab_travel_x_ = 0.0F;
    grab_travel_y_ = 0.0F;
    reset_requests_ = 0U;
    view_reset_requests_ = 0U;
    free_toggle_requests_ = 0U;
    free_look_yaw_units_ = 0.0F;
    free_look_pitch_units_ = 0.0F;
}

void Adapter::pointer_left() {
    pointer_valid_ = false;
    for (auto held = pressed_.begin(); held != pressed_.end();) {
        if (held->first.device != Device::mouse_button) {
            ++held;
            continue;
        }
        // Releasing a control this adapter pressed cannot be refused.
        static_cast<void>(reducer_.process(camera::Event{
            static_cast<camera::ControlId>(held->second + 1U),
            static_cast<camera::ActionId>(table_->bindings[held->second].action),
            camera::EventType::release}));
        held = pressed_.erase(held);
    }
    rotate_units_ = 0.0F;
    orbit_pitch_units_ = 0.0F;
    translate_x_ = 0.0F;
    translate_y_ = 0.0F;
    pan_motion_x_ = 0.0F;
    pan_motion_y_ = 0.0F;
    grab_travel_x_ = 0.0F;
    grab_travel_y_ = 0.0F;
    free_look_yaw_units_ = 0.0F;
    free_look_pitch_units_ = 0.0F;
}

core::Result<void> Adapter::activate(const std::string_view json) {
    auto parsed = parse_binding_table(json);
    if (!parsed) return core::Result<void>::failure(parsed.error());
    table_ = std::move(parsed).value();
    cancel();
    return core::Result<void>::success();
}

void Adapter::set_context(const Context context) noexcept {
    if (context_ == context) return;
    context_ = context;
    cancel();
}

void Adapter::set_focus(const bool focused) noexcept {
    if (reducer_.focus_eligible() == focused) return;
    reducer_.set_focus_eligible(focused);
    if (!focused) cancel();
}

void Adapter::set_capture_locked(const bool locked) noexcept {
    if (reducer_.capture_locked() == locked) return;
    reducer_.set_capture_locked(locked);
    if (locked) cancel();
}

core::Result<void> Adapter::set_viewport(const float width, const float height) {
    if (!finite(width) || !finite(height) || width < 0.0F || height < 0.0F) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_adapter_input,
            "viewport extent must be finite and non-negative"));
    }
    // Every published extent is a new generation, even an identical one after
    // a restore: pointer samples and drag anchors from before are stale.
    viewport_width_ = width;
    viewport_height_ = height;
    viewport_known_ = true;
    ++counters_.viewport_generation;
    cancel();
    return core::Result<void>::success();
}

std::optional<std::size_t> Adapter::find_binding(
    const Device device, const std::uint32_t code, const std::uint8_t modifiers) const noexcept {
    if (!table_) return std::nullopt;
    for (std::size_t index = 0; index < table_->bindings.size(); ++index) {
        const Binding& binding = table_->bindings[index];
        if (binding.context == context_ && binding.device == device && binding.code == code
            && binding.modifiers == modifiers) {
            return index;
        }
    }
    return std::nullopt;
}

core::Result<void> Adapter::handle_button(const Device device, const RawEvent& event) {
    const PhysicalControl physical{device, event.code};
    const auto held = pressed_.find(physical);
    if (!event.pressed) {
        // A release always resolves against what was pressed, whatever the
        // modifiers are now. A release with nothing pressed is inert.
        if (held == pressed_.end()) return core::Result<void>::success();
        const Binding& binding = table_->bindings[held->second];
        const auto released = reducer_.process(camera::Event{
            static_cast<camera::ControlId>(held->second + 1U),
            static_cast<camera::ActionId>(binding.action), camera::EventType::release});
        if (!released) return released;
        // FoC: a middle release that barely moved, without Ctrl at release,
        // resets the view. Ctrl is read at release, as in the original.
        if (table_->click_reset && binding.action == Action::rotate_grab
            && device == Device::mouse_button && (event.modifiers & modifier::ctrl) == 0U
            && std::hypot(grab_travel_x_ / viewport_width_, grab_travel_y_ / viewport_height_)
                <= click_travel_screen_fraction) {
            ++view_reset_requests_;
        }
        pressed_.erase(held);
        ++counters_.routed;
        return core::Result<void>::success();
    }
    if (held != pressed_.end()) {
        // OS key repeat or a duplicate press: never a new contribution and
        // never a second pressed-trigger request.
        const Binding& binding = table_->bindings[held->second];
        const auto repeated = reducer_.process(camera::Event{
            static_cast<camera::ControlId>(held->second + 1U),
            static_cast<camera::ActionId>(binding.action), camera::EventType::repeat});
        if (!repeated) return repeated;
        return core::Result<void>::success();
    }
    if (event.echo) {
        // An echo for a control whose press was cancelled is stale.
        ++counters_.ignored_ineligible;
        return core::Result<void>::success();
    }
    const auto index = find_binding(device, event.code, event.modifiers);
    if (!index) {
        ++counters_.ignored_unbound;
        return core::Result<void>::success();
    }
    const Binding& binding = table_->bindings[*index];
    const auto pressed = reducer_.process(camera::Event{
        static_cast<camera::ControlId>(*index + 1U),
        static_cast<camera::ActionId>(binding.action), camera::EventType::press});
    if (!pressed) return pressed;
    pressed_.emplace(physical, *index);
    if (binding.action == Action::rotate_grab) {
        grab_travel_x_ = 0.0F;
        grab_travel_y_ = 0.0F;
    }
    if (binding.trigger == Trigger::pressed) {
        if (binding.action == Action::zoom) zoom_detents_ += binding.scale;
        if (binding.action == Action::reset_view) ++reset_requests_;
        if (binding.action == Action::free_toggle) ++free_toggle_requests_;
    }
    ++counters_.routed;
    return core::Result<void>::success();
}

core::Result<void> Adapter::handle(const RawEvent& event) {
    if (!finite(event.factor) || !finite(event.relative_x) || !finite(event.relative_y)
        || !finite(event.position_x) || !finite(event.position_y)
        || (event.modifiers & ~modifier::all) != 0U) {
        return core::Result<void>::failure(failure(diagnostic_codes::invalid_adapter_input,
            "raw camera event values must be finite with known modifier bits"));
    }
    if (!table_) {
        return core::Result<void>::failure(failure(diagnostic_codes::no_active_bindings,
            "no camera binding table is active"));
    }
    ++counters_.delivered;
    if (!eligible()) {
        ++counters_.ignored_ineligible;
        return core::Result<void>::success();
    }

    switch (event.kind) {
    case RawKind::key:
        return handle_button(Device::keyboard, event);
    case RawKind::mouse_button:
        if (event.has_position) {
            pointer_x_ = event.position_x;
            pointer_y_ = event.position_y;
            pointer_valid_ = true;
        }
        return handle_button(Device::mouse_button, event);
    case RawKind::mouse_wheel: {
        if (!event.pressed) return core::Result<void>::success();
        // PI-9: a held middle grab suppresses the wheel, without deferring it.
        if (reducer_.is_held(static_cast<camera::ActionId>(Action::rotate_grab))) {
            ++counters_.ignored_ineligible;
            return core::Result<void>::success();
        }
        const auto index = find_binding(Device::mouse_wheel, event.code, event.modifiers);
        if (!index) {
            ++counters_.ignored_unbound;
            return core::Result<void>::success();
        }
        const float notches = event.factor > 0.0F ? event.factor : 1.0F;
        zoom_detents_ += table_->bindings[*index].scale * notches;
        ++counters_.routed;
        return core::Result<void>::success();
    }
    case RawKind::mouse_motion: {
        if (event.has_position) {
            pointer_x_ = event.position_x;
            pointer_y_ = event.position_y;
            pointer_valid_ = true;
        }
        // Relative rotation or look only while a grab started in this
        // generation is held; the grab itself was cancelled by any lifecycle
        // change, including entering or leaving free flight.
        const bool rotating = reducer_.is_held(static_cast<camera::ActionId>(Action::rotate_grab));
        const bool looking =
            reducer_.is_held(static_cast<camera::ActionId>(Action::free_look_grab));
        const bool panning = context_ != Context::free;
        if (!rotating && !looking && !panning) return core::Result<void>::success();
        if (rotating) {
            grab_travel_x_ += event.relative_x;
            grab_travel_y_ += event.relative_y;
        }
        bool routed = false;
        const auto route_axis = [&](const std::uint32_t axis, const float relative) {
            const auto index = find_binding(Device::mouse_motion, axis, event.modifiers);
            if (!index) return;
            const Binding& binding = table_->bindings[*index];
            if (binding.action == Action::rotate && rotating) {
                rotate_units_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::orbit_pitch && rotating) {
                orbit_pitch_units_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::translate_x && rotating) {
                translate_x_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::translate_y && rotating) {
                translate_y_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::pan_motion_x && panning) {
                pan_motion_x_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::pan_motion_y && panning) {
                pan_motion_y_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::free_look_yaw && looking) {
                free_look_yaw_units_ += binding.scale * relative;
                routed = true;
            } else if (binding.action == Action::free_look_pitch && looking) {
                free_look_pitch_units_ += binding.scale * relative;
                routed = true;
            }
        };
        route_axis(mouse_code::motion_x, event.relative_x);
        route_axis(mouse_code::motion_y, event.relative_y);
        if (routed) ++counters_.routed;
        return core::Result<void>::success();
    }
    }
    return core::Result<void>::failure(failure(diagnostic_codes::invalid_adapter_input,
        "unknown raw camera event kind"));
}

core::Result<StepIntent> Adapter::take_step(const camera::Constants& constants) {
    StepIntent intent;
    if (!eligible()) {
        // Nothing accumulated while ineligible, but clear defensively so a
        // pending value can never outlive the eligibility that produced it.
        clear_pending();
        return core::Result<StepIntent>::success(intent);
    }
    const auto held = [this](const Action action) {
        return reducer_.is_held(static_cast<camera::ActionId>(action)) ? 1.0F : 0.0F;
    };
    // Opposing actions cancel; apply_pan normalises the final vector once.
    intent.pan_x = held(Action::pan_right) - held(Action::pan_left);
    intent.pan_y = held(Action::pan_forward) - held(Action::pan_back);
    // Pointer pan is a drag displacement in viewport heights, not a velocity:
    // pixels per frame already scale with the frame time. Eligibility
    // guarantees a nonzero viewport.
    intent.drag_x = pan_motion_x_ / viewport_height_;
    intent.drag_y = pan_motion_y_ / viewport_height_;
    intent.push_scroll = held(Action::push_scroll) > 0.0F;
    intent.free_move_x = held(Action::free_move_right) - held(Action::free_move_left);
    intent.free_move_y = held(Action::free_rise) - held(Action::free_descend);
    intent.free_move_z = held(Action::free_move_forward) - held(Action::free_move_back);
    // Edge scrolling is a tactical pan; free flight never edge scrolls.
    if (context_ != Context::free && table_->edge_scroll && pointer_valid_) {
        auto edge = camera::edge_scroll(
            constants, pointer_x_, pointer_y_, viewport_width_, viewport_height_);
        if (!edge) return core::Result<StepIntent>::failure(edge.error());
        // Edge axes are screen pixels (+y down); pan +y is up the screen.
        if (intent.pan_x == 0.0F) intent.pan_x = static_cast<float>(edge.value().axis_x);
        if (intent.pan_y == 0.0F) intent.pan_y = -static_cast<float>(edge.value().axis_y);
    }
    intent.zoom_detents = zoom_detents_;
    // FoC mouse units: screen fractions (x of the width, y of the height)
    // times 100, so the same share of the screen turns the same at any size.
    // Only tables that opt in; the others keep one unit per scaled pixel.
    const bool screen = table_->screen_mouse_units;
    const float per_width = screen ? mouse_units_per_screen / viewport_width_ : 1.0F;
    const float per_height = screen ? mouse_units_per_screen / viewport_height_ : 1.0F;
    intent.rotate_units = rotate_units_ * per_width;
    intent.orbit_pitch_units = orbit_pitch_units_ * per_height;
    intent.translate_x = translate_x_ * per_width;
    intent.translate_y = translate_y_ * per_height;
    intent.reset_requests = reset_requests_;
    intent.view_reset_requests = view_reset_requests_;
    intent.free_toggle_requests = free_toggle_requests_;
    intent.free_look_yaw_units = free_look_yaw_units_;
    intent.free_look_pitch_units = free_look_pitch_units_;
    // The grab travel belongs to the held grab, not to this step.
    const float travel_x = grab_travel_x_;
    const float travel_y = grab_travel_y_;
    clear_pending();
    grab_travel_x_ = travel_x;
    grab_travel_y_ = travel_y;
    return core::Result<StepIntent>::success(intent);
}

core::Result<TacticalPose> advance_pose(
    const camera::Constants& constants, const TacticalPose& pose, const StepIntent& intent,
    const float default_zoom, const float delta_seconds) {
    if (!finite(delta_seconds) || delta_seconds < 0.0F || !finite(default_zoom)
        || !finite(intent.pan_x) || !finite(intent.pan_y) || !finite(intent.drag_x)
        || !finite(intent.drag_y) || !finite(intent.zoom_detents)
        || !finite(intent.rotate_units) || !finite(pose.target[0]) || !finite(pose.target[1])
        || !finite(pose.target[2])) {
        return core::Result<TacticalPose>::failure(failure(
            diagnostic_codes::invalid_adapter_input,
            "camera step needs a finite non-negative duration and finite intent/pose"));
    }
    float zoom = pose.state.zoom;
    float yaw = pose.state.yaw_degrees;
    if (intent.reset_requests > 0U) {
        auto clamped = camera::clamp_zoom(default_zoom);
        if (!clamped) return core::Result<TacticalPose>::failure(clamped.error());
        zoom = clamped.value();
        yaw = std::clamp(constants.yaw_default, constants.yaw_min, constants.yaw_max);
    }
    if (intent.zoom_detents != 0.0F) {
        auto stepped = camera::apply_zoom_step(constants, zoom, intent.zoom_detents);
        if (!stepped) return core::Result<TacticalPose>::failure(stepped.error());
        zoom = stepped.value();
    }
    if (intent.rotate_units != 0.0F) {
        auto rotated = camera::apply_rotate(constants, yaw, intent.rotate_units);
        if (!rotated) return core::Result<TacticalPose>::failure(rotated.error());
        yaw = rotated.value();
    }
    auto solved = camera::solve(constants, zoom);
    if (!solved) return core::Result<TacticalPose>::failure(solved.error());
    TacticalPose next;
    next.state = solved.value();
    next.state.yaw_degrees = yaw;
    next.target = pose.target;

    auto pan = camera::apply_pan(constants, next.state,
        camera::PanInput{intent.pan_x, intent.pan_y, intent.push_scroll}, delta_seconds);
    if (!pan) return core::Result<TacticalPose>::failure(pan.error());
    // The pointer drag is a displacement, applied once whatever the duration.
    auto dragged = camera::drag_displacement(next.state, intent.drag_x, intent.drag_y);
    if (!dragged) return core::Result<TacticalPose>::failure(dragged.error());
    pan.value().x += dragged.value().x;
    pan.value().y += dragged.value().y;
    if (pan.value().x != 0.0F || pan.value().y != 0.0F) {
        constexpr float degrees_to_radians = 3.14159265358979323846F / 180.0F;
        const float radians = yaw * degrees_to_radians;
        const float cosine = std::cos(radians);
        const float sine = std::sin(radians);
        // right = (cos, -sin) and forward-on-screen = (-sin, -cos) in (X, Z).
        next.target[0] += pan.value().x * cosine - pan.value().y * sine;
        next.target[2] += -pan.value().x * sine - pan.value().y * cosine;
    }
    return core::Result<TacticalPose>::success(next);
}

} // namespace eawr::viewer::camera_input
