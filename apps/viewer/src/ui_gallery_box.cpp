#include "ui_gallery_internal.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

// `--eawr-message-box <variant>` (MNU-42): the runtime message box on the gallery's skin, with its
// results and click blocking checked through Godot's real input dispatch.
namespace eawr::presentation::godot_backend {
namespace {

constexpr int frames_per_step = 3;

// <none|one|two|three|small|quit>[-centred]: demo contents for the box under test. The texts are
// keys of the stock text database; a missing key shows as written.
[[nodiscard]] std::optional<model::MessageBoxRequest> parse_variant(std::string variant) {
    model::MessageBoxRequest request;
    constexpr std::string_view suffix = "-centred";
    if (variant.size() > suffix.size() && variant.compare(variant.size() - suffix.size(), suffix.size(), suffix) == 0) {
        request.centred = true;
        variant.resize(variant.size() - suffix.size());
    }
    request.caption = "TEXT_QUIT_SURE";
    request.lines[0] = "Message box line one";
    request.lines[1] = "Message box line two";
    const model::BoxButton yes{"TEXT_BUTTON_YES", 1, false};
    const model::BoxButton no{"TEXT_BUTTON_NO", 0, true};
    const model::BoxButton cancel{"TEXT_BUTTON_CANCEL", 2, true};
    if (variant == "none") {
        // no buttons
    } else if (variant == "one") {
        request.buttons = {yes};
    } else if (variant == "two") {
        request.buttons = {yes, no};
    } else if (variant == "three") {
        request.buttons = {yes, no, cancel};
    } else if (variant == "small") {
        request.kind = model::BoxKind::small_message;
        request.caption = "TEXT_SAVELOAD_LOADING_MSG";
        request.lines = {};
    } else if (variant == "quit") {
        request.kind = model::BoxKind::quit_confirm;
        request.buttons = {yes, no};
    } else {
        return std::nullopt;
    }
    return request;
}

void send_motion(Viewport& viewport, const Vector2 local) {
    const Vector2 window = viewport.get_final_transform().xform(local);
    Ref<InputEventMouseMotion> event;
    event.instantiate();
    event->set_position(window);
    event->set_global_position(window);
    event->set_relative(Vector2(1.0F, 0.0F));
    Input::get_singleton()->parse_input_event(event);
}

void send_click(Viewport& viewport, const Vector2 local) {
    send_motion(viewport, local);
    const Vector2 window = viewport.get_final_transform().xform(local);
    for (const bool pressed : {true, false}) {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(MOUSE_BUTTON_LEFT);
        event->set_pressed(pressed);
        event->set_factor(1.0F);
        event->set_position(window);
        event->set_global_position(window);
        Input::get_singleton()->parse_input_event(event);
    }
}

void send_escape() {
    for (const bool pressed : {true, false}) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(KEY_ESCAPE);
        event->set_physical_keycode(KEY_ESCAPE);
        event->set_pressed(pressed);
        Input::get_singleton()->parse_input_event(event);
    }
}

} // namespace

UiGalleryMode::State* UiGalleryMode::State::box_active = nullptr;

void UiGalleryMode::State::on_box_probe() {
    if (box_active != nullptr) ++box_active->box_probe_presses;
}

void UiGalleryMode::State::on_box_result(const int64_t result) {
    if (box_active != nullptr) box_active->box_results.push_back(result);
}

bool UiGalleryMode::State::box_alive() {
    if (box != nullptr && !UtilityFunctions::is_instance_id_valid(box_id)) box = nullptr;
    return box != nullptr;
}

void UiGalleryMode::State::open_box() {
    box = memnew(EawrMessageBox);
    if (!box->open(*box_parent, box_context, box_request)) {
        failure = box->failure();
        memdelete(box);
        box = nullptr;
        return;
    }
    box_id = static_cast<int64_t>(box->get_instance_id());
    box->connect("result_chosen", callable_mp_static(&State::on_box_result));
}

void UiGalleryMode::State::build_box_page(Node& parent, Control& root) {
    auto request = parse_variant(options.message_box);
    if (!request) {
        failure = "--eawr-message-box expects none, one, two, three, small or quit, with an optional -centred";
        return;
    }
    box_request = std::move(*request);
    box_context = MessageBoxContext{&*catalog, &*theme_model, text_lookup ? &*text_lookup : nullptr, theme,
                                    Vector2(static_cast<float>(width), static_cast<float>(height))};
    box_parent = &parent;
    box_active = this;

    // The "world" under the box: a button in the corner and one over the middle of the screen, where
    // the box's frame lands. Neither may ever see a click while the box is open.
    const auto probe = [&root](const Rect2& rect) {
        auto* button = memnew(Button);
        button->set_flat(true);
        button->set_focus_mode(Control::FOCUS_NONE);
        button->set_position(rect.position);
        button->set_size(rect.size);
        button->connect("pressed", callable_mp_static(&State::on_box_probe));
        root.add_child(button);
    };
    probe(Rect2(0.0F, 0.0F, 160.0F, 90.0F));
    probe(Rect2(static_cast<float>(width) * 0.5F - 300.0F, static_cast<float>(height) * 0.5F - 70.0F, 600.0F, 140.0F));

    open_box();
    if (!failure.empty()) return;
    const model::MessageBoxPlan plan = model::plan_message_box(box_request);
    Viewport* viewport = parent.get_viewport();

    const auto unmoved = [this]() -> std::string {
        if (box_probe_presses != 0) return "a click reached the world under the box";
        if (!box_results.empty()) return "a click outside the buttons chose a result";
        return box_alive() ? std::string{} : "the box closed";
    };
    box_steps.push_back({"a click beside the box is blocked",
                         [viewport] { send_click(*viewport, Vector2(40.0F, 40.0F)); }, unmoved});
    // Inside the frame, in its top border and away from every gadget.
    box_steps.push_back({"a click on the frame away from its buttons is blocked",
                         [this, viewport] {
                             if (!box_alive()) return;
                             const Vector2 position = box->frame()->get_position();
                             send_click(*viewport, position + Vector2(box->frame()->get_size().x * 0.5F, 3.0F));
                         },
                         unmoved});
    for (const model::PlannedBoxButton& planned : plan.buttons) {
        box_steps.push_back({"the " + planned.control + " press gives its result",
                             [this, viewport, control = planned.control] {
                                 box_results.clear();
                                 Control* target = box_alive() ? box->button(control) : nullptr;
                                 if (target != nullptr) send_click(*viewport, target->get_global_rect().get_center());
                             },
                             [this, result = planned.result]() -> std::string {
                                 if (box_results.size() != 1) {
                                     return "expected one result, saw " + std::to_string(box_results.size());
                                 }
                                 if (box_results[0] != result) {
                                     return "result " + std::to_string(box_results[0]) + ", expected "
                                         + std::to_string(result);
                                 }
                                 if (box_alive()) return "the box stayed open after a choice";
                                 return box_probe_presses == 0 ? std::string{} : "a click reached the world";
                             }});
        box_steps.push_back({"the box opens again", [this] { open_box(); },
                             [this]() -> std::string { return box_alive() ? std::string{} : "the box did not open"; }});
    }
    // Esc: the cancel result where the box has one, nothing otherwise.
    box_steps.push_back({"Esc gives the cancel result or nothing",
                         [this] {
                             box_results.clear();
                             send_escape();
                         },
                         [this, escape = plan.escape_result()]() -> std::string {
                             if (escape) {
                                 if (box_results.size() != 1 || box_results[0] != *escape) {
                                     return "Esc did not give result " + std::to_string(*escape);
                                 }
                                 return box_alive() ? "the box stayed open after Esc" : std::string{};
                             }
                             if (!box_results.empty()) return "Esc chose a result in a box with no cancel button";
                             return box_alive() ? std::string{} : "Esc closed a box with no cancel button";
                         }});

    // The open box is sampled for the capture's colours.
    if (const data::Dialog* found = catalog->script.find(plan.dialog)) {
        samples.push_back({"dialog", "normal", theme_model->variation(*found, nullptr), box->frame(),
                           Rect2(box->frame()->get_position(), box->frame()->get_size()), 0U});
    }
}

bool UiGalleryMode::State::run_box_script() {
    if (!box_captured) {
        box_captured = true;
        // The open box, before any click, is the capture.
        if (!capture()) return true;
    }
    if (box_step >= box_steps.size()) return true;
    if (box_wait > 0) {
        --box_wait;
        return false;
    }
    if (std::chrono::steady_clock::now() < box_wait_until) return false;
    BoxStep& step = box_steps[box_step];
    if (box_acting) {
        step.act();
        box_acting = false;
        box_wait = frames_per_step;
        box_wait_until = std::chrono::steady_clock::now() + step.wait_time;
        return false;
    }
    box_step_results.push_back({step.name, step.check()});
    box_acting = true;
    ++box_step;
    return false;
}

} // namespace eawr::presentation::godot_backend
