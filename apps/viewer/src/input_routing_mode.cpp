#include "input_routing_mode.hpp"

#include "capture_viewport.hpp"
#include "viewer_path.hpp"

#include "ui/input_routing.hpp"

#include "eawr/presentation/ui/command_sink.hpp"
#include "eawr/presentation/ui/hud.hpp"
#include "eawr/presentation/ui/layout.hpp"

#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/canvas_layer.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/line_edit.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace eawr::presentation::godot_backend {

using namespace godot;

namespace {

namespace ui = presentation::ui;
namespace tactical = sim::tactical;

constexpr tactical::PlayerId local_player = 1;
constexpr int settle_frames = 5;
constexpr int frames_per_step = 3;
constexpr std::uint32_t width = 1280;
constexpr std::uint32_t height = 720;

[[nodiscard]] std::string utf8(const String& value) {
    const CharString converted = value.utf8();
    return std::string(converted.get_data(), converted.length());
}

struct Counts {
    std::uint64_t press{}, release{}, motion{}, key{}, wheel{};
    [[nodiscard]] Counts minus(const Counts& before) const {
        return {press - before.press, release - before.release, motion - before.motion, key - before.key,
            wheel - before.wheel};
    }
    [[nodiscard]] std::string text() const {
        return "press " + std::to_string(press) + ", release " + std::to_string(release) + ", motion "
            + std::to_string(motion) + ", key " + std::to_string(key) + ", wheel " + std::to_string(wheel);
    }
    friend bool operator==(const Counts&, const Counts&) = default;
};

void count(Counts& counts, const ui::InputClass input) {
    switch (input) {
    case ui::InputClass::key: ++counts.key; break;
    case ui::InputClass::button_press: ++counts.press; break;
    case ui::InputClass::button_release: ++counts.release; break;
    case ui::InputClass::wheel: ++counts.wheel; break;
    case ui::InputClass::motion: ++counts.motion; break;
    }
}

[[nodiscard]] std::string_view origin_name(const ui::CommandOrigin origin) {
    switch (origin) {
    case ui::CommandOrigin::hud_button: return "hud_button";
    case ui::CommandOrigin::hotkey: return "hotkey";
    case ui::CommandOrigin::world_click: return "world_click";
    case ui::CommandOrigin::minimap: return "minimap";
    }
    return "unknown";
}

// A second CommandSink implementation, as the live session bridge (#80) will write one: it
// forwards to the standard scheduler and keeps the presentation-only origin for the report.
class RecordingSink final : public ui::CommandSink {
public:
    [[nodiscard]] core::Result<void> issue(const ui::TacticalIntent& intent) override {
        auto issued = scheduler.issue(intent);
        if (issued) origins.push_back(intent.origin);
        return issued;
    }
    ui::CommandScheduler scheduler{local_player};
    std::vector<ui::CommandOrigin> origins;
};

struct Logged {
    tactical::PlayerCommand command;
    ui::CommandOrigin origin{};
};

struct Step {
    std::string name;
    std::string rule;
    std::function<void()> act;
    std::function<std::string()> check; // empty when the rule held
};

struct StepResult {
    std::string name;
    std::string rule;
    std::string problem;
};

enum Control_ : int { stop_button = 0, move_button = 1, modal_close = 2 };

} // namespace

struct InputRoutingMode::State {
    Options options;
    Node3D* host{};
    Viewport* viewport{};
    ui::HudViewModel hud;
    ui::ReferenceSpace space;
    ui::ShellPlacement placement;
    EawrUiHitMask* mask{};
    Button* stop{};
    Button* move{};
    LineEdit* edit{};
    EawrUiModalLayer* modal{};
    Button* close{};
    RecordingSink sink;
    ui::OrderInput orders{sink};
    Counts world;
    Counts dropped;
    // What the world layer holds: keys and mouse buttons it saw pressed and not released or
    // cancelled. A key echo or a release for something it does not hold is ignored, as in the
    // camera adapter.
    std::set<std::int64_t> held_keys;
    std::set<std::uint32_t> held_buttons;
    std::uint64_t cancels{};
    std::uint64_t cancels_before{};
    Counts before;
    Counts dropped_before;
    std::uint64_t stop_presses{};
    std::uint64_t move_presses{};
    std::vector<std::string> order_failures;
    std::vector<Step> steps;
    std::vector<StepResult> results;
    std::vector<Logged> commands;
    std::size_t step{};
    int wait{settle_frames};
    bool acting{true};
    bool completed{};
    std::string failure;

    static State* active;
    static void on_button(const int64_t which) {
        if (active == nullptr) return;
        switch (which) {
        case stop_button:
            ++active->stop_presses;
            if (auto stopped = active->orders.stop(ui::CommandOrigin::hud_button); !stopped) {
                active->order_failures.push_back(core::format_diagnostic(stopped.error()));
            }
            break;
        case move_button:
            ++active->move_presses;
            active->orders.arm(ui::OrderMode::move);
            break;
        case modal_close: active->modal->hide(); break;
        default: break;
        }
    }

    // A shell reference point (origin lower left, y up) in viewport pixels.
    [[nodiscard]] Vector2 screen(const double x, const double y) const {
        return {static_cast<float>(placement.left + x * placement.scale),
            static_cast<float>(placement.bottom - y * placement.scale)};
    }
    [[nodiscard]] static Vector2 centre(const Control& control) {
        return control.get_global_rect().get_center();
    }
    // Viewport pixels to the window coordinates Input::parse_input_event expects.
    [[nodiscard]] Vector2 window(const Vector2 local) const { return viewport->get_final_transform().xform(local); }

    void button(const MouseButton index, const bool pressed, const Vector2 local) const {
        Ref<InputEventMouseButton> event;
        event.instantiate();
        event->set_button_index(index);
        event->set_pressed(pressed);
        event->set_factor(1.0F);
        event->set_position(window(local));
        event->set_global_position(window(local));
        Input::get_singleton()->parse_input_event(event);
    }
    void click(const MouseButton index, const Vector2 local) const {
        motion(local);
        button(index, true, local);
        button(index, false, local);
    }
    void motion(const Vector2 local) const {
        Ref<InputEventMouseMotion> event;
        event.instantiate();
        event->set_position(window(local));
        event->set_global_position(window(local));
        event->set_relative(Vector2(1.0F, 0.0F));
        Input::get_singleton()->parse_input_event(event);
    }
    static void key(const Key code, const char32_t unicode, const bool pressed, const bool echo = false) {
        Ref<InputEventKey> event;
        event.instantiate();
        event->set_keycode(code);
        event->set_physical_keycode(code);
        event->set_unicode(unicode);
        event->set_pressed(pressed);
        event->set_echo(echo);
        Input::get_singleton()->parse_input_event(event);
    }
    static void tap(const Key code, const char32_t unicode = 0) {
        key(code, unicode, true);
        key(code, unicode, false);
    }

    [[nodiscard]] Counts world_delta() const { return world.minus(before); }
    [[nodiscard]] Counts dropped_delta() const { return dropped.minus(dropped_before); }
    [[nodiscard]] std::uint64_t cancels_delta() const { return cancels - cancels_before; }
    [[nodiscard]] std::string cancelled_text() const {
        return "the host cancelled held input " + std::to_string(cancels_delta()) + " times, expected once";
    }
    [[nodiscard]] std::string holds() const {
        return std::to_string(held_keys.size()) + " keys and " + std::to_string(held_buttons.size()) + " buttons";
    }

    bool build(Node3D& node);
    void script();
    void drain();
    [[nodiscard]] bool write_report(bool passed) const;
};

InputRoutingMode::State* InputRoutingMode::State::active = nullptr;

bool InputRoutingMode::requested() {
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-input-routing") return true;
    }
    return false;
}

InputRoutingMode::Options InputRoutingMode::from_command_line() {
    Options options;
    const PackedStringArray arguments = OS::get_singleton()->get_cmdline_user_args();
    for (int64_t index = 0; index + 1 < arguments.size(); ++index) {
        if (utf8(arguments[index]) == "--eawr-report") {
            options.report_path = ViewerPath{utf8(arguments[index + 1])}.native();
            ++index;
        }
    }
    return options;
}

InputRoutingMode::InputRoutingMode(Options options) : state_(std::make_unique<State>()) {
    state_->options = std::move(options);
}
InputRoutingMode::~InputRoutingMode() {
    if (state_ && State::active == state_.get()) State::active = nullptr;
}
InputRoutingMode::InputRoutingMode(InputRoutingMode&&) noexcept = default;
InputRoutingMode& InputRoutingMode::operator=(InputRoutingMode&&) noexcept = default;

bool InputRoutingMode::State::build(Node3D& node) {
    host = &node;
    viewport = node.get_viewport();
    // A fixed 1280 x 720 viewport whatever window the OS (or --headless) gives; injected
    // events are mapped into it through the window's final transform.
    pin_capture_viewport(*node.get_window(), width, height);
    const Vector2 size = viewport->get_visible_rect().size;
    if (static_cast<std::uint32_t>(size.x) != width || static_cast<std::uint32_t>(size.y) != height) {
        failure = "the viewport is " + std::to_string(size.x) + " x " + std::to_string(size.y) + ", not 1280 x 720";
        return false;
    }

    // A synthetic 800 x 200 shell: one faceplate over all of it whose alpha mask is opaque on
    // the left half only, and two component rects on the transparent half.
    auto alpha = std::make_shared<ui::ShellAlphaMask>();
    alpha->width = 2;
    alpha->height = 1;
    alpha->alpha = {255, 0};
    const auto vertex = [](const float x, const float y) {
        return data::ui::ShellVertex{{x, y, 0.0F}, {x / 800.0F, y / 200.0F}};
    };
    hud.visible_extent = data::ui::ReferenceRect{0, 0, 800, 200};
    hud.faceplates.push_back({{{vertex(0, 0), vertex(800, 0), vertex(800, 200)}, "synthetic"}, alpha});
    hud.faceplates.push_back({{{vertex(0, 0), vertex(800, 200), vertex(0, 200)}, "synthetic"}, alpha});
    hud.components.push_back({"stop", {500, 60, 100, 80}, true});
    hud.components.push_back({"move", {650, 60, 100, 80}, true});
    space = ui::reference_space({width, height});
    placement = hud.placement(space);

    CanvasLayer* layer = memnew(CanvasLayer);
    node.add_child(layer);
    mask = memnew(EawrUiHitMask);
    mask->set_hit_test([this](const double x, const double y) { return hud.hit_test_screen({x, y}, space); });
    layer->add_child(mask);
    mask->set_position(Vector2());
    mask->set_size(size);
    const auto place = [&](Control& control, const data::ui::ReferenceRect& rect) {
        const Vector2 top_left = screen(rect.x, rect.top());
        control.set_position(top_left);
        control.set_size(Vector2(static_cast<float>(rect.width * placement.scale),
            static_cast<float>(rect.height * placement.scale)));
    };
    // Like the kit's buttons, HUD buttons never take keyboard focus, so arrow keys stay the
    // camera's after a click.
    stop = memnew(Button);
    stop->set_focus_mode(Control::FOCUS_NONE);
    stop->set_text("Stop");
    mask->add_child(stop);
    place(*stop, hud.components[0].rect);
    move = memnew(Button);
    move->set_focus_mode(Control::FOCUS_NONE);
    move->set_text("Move");
    mask->add_child(move);
    place(*move, hud.components[1].rect);
    edit = memnew(LineEdit);
    layer->add_child(edit);
    edit->set_position(Vector2(20.0F, 20.0F));
    edit->set_size(Vector2(240.0F, 32.0F));
    modal = memnew(EawrUiModalLayer);
    layer->add_child(modal);
    modal->set_position(Vector2());
    modal->set_size(size);
    close = memnew(Button);
    close->set_text("Close");
    modal->add_child(close);
    close->set_position(size * 0.5F - Vector2(60.0F, 20.0F));
    close->set_size(Vector2(120.0F, 40.0F));
    modal->hide();

    active = this;
    stop->connect("pressed", callable_mp_static(&State::on_button).bind(static_cast<int64_t>(stop_button)));
    move->connect("pressed", callable_mp_static(&State::on_button).bind(static_cast<int64_t>(move_button)));
    close->connect("pressed", callable_mp_static(&State::on_button).bind(static_cast<int64_t>(modal_close)));
    node.set_process_input(true);
    node.set_process_unhandled_input(true);
    script();
    return true;
}

void InputRoutingMode::State::script() {
    // Screen points: a world point above the HUD, an opaque and a transparent faceplate point.
    const Vector2 world_point(640.0F, 150.0F);
    const Vector2 opaque = screen(200, 100);
    const Vector2 clear = screen(450, 100);
    const auto none = [this](const char* what) -> std::string {
        const Counts delta = world_delta();
        return delta == Counts{} ? std::string{} : std::string(what) + " reached the world: " + delta.text();
    };
    steps.push_back({"transparent faceplate passes to the world", "UI-I2", [=, this] { click(MOUSE_BUTTON_LEFT, clear); },
        [this] {
            const Counts d = world_delta();
            return d.press == 1 && d.release == 1 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"opaque faceplate stops the pointer", "UI-I2", [=, this] { click(MOUSE_BUTTON_LEFT, opaque); },
        [=] { return none("a click on an opaque faceplate texel"); }});
    steps.push_back({"HUD button takes its click first", "UI-I1",
        [this] {
            orders.set_selection({2, 1});
            click(MOUSE_BUTTON_LEFT, centre(*stop));
        },
        [=, this] {
            if (stop_presses != 1) return "the Stop button saw " + std::to_string(stop_presses) + " presses";
            return none("a HUD button click");
        }});
    steps.push_back({"keys still reach the world after a HUD click", "UI-I1", [] { tap(KEY_LEFT); },
        [this] { return world_delta().key == 2 ? std::string{} : "world saw " + world_delta().text(); }});
    steps.push_back({"the wheel over the HUD stops there", "UI-I2",
        [=, this] {
            motion(opaque);
            button(MOUSE_BUTTON_WHEEL_UP, true, opaque);
            button(MOUSE_BUTTON_WHEEL_UP, false, opaque);
            motion(centre(*stop));
            button(MOUSE_BUTTON_WHEEL_DOWN, true, centre(*stop));
            button(MOUSE_BUTTON_WHEEL_DOWN, false, centre(*stop));
        },
        [=] { return none("the wheel over the HUD"); }});
    steps.push_back({"world order click", "UI-I1", [=, this] { click(MOUSE_BUTTON_RIGHT, world_point); },
        [this] {
            const Counts d = world_delta();
            return d.press == 1 && d.release == 1 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"world hotkey", "UI-I1", [] { tap(KEY_S, U's'); },
        [this] { return world_delta().key == 2 ? std::string{} : "world saw " + world_delta().text(); }});
    steps.push_back({"HUD move mode then world click", "UI-C1",
        [=, this] {
            click(MOUSE_BUTTON_LEFT, centre(*move));
            click(MOUSE_BUTTON_LEFT, world_point);
        },
        [this] {
            const Counts d = world_delta();
            if (move_presses != 1) return std::string("the Move button was not pressed");
            return d.press == 1 && d.release == 1 && orders.mode() == ui::OrderMode::none
                ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"clicking the edit box focuses it", "UI-I3", [this] { click(MOUSE_BUTTON_LEFT, centre(*edit)); },
        [=, this] {
            if (viewport->gui_get_focus_owner() != edit) return std::string("the edit box did not take focus");
            return none("a click on the edit box");
        }});
    steps.push_back({"a focused edit box suppresses hotkeys and camera keys", "UI-I3",
        [] {
            tap(KEY_S, U's');
            tap(KEY_W, U'w');
            tap(KEY_F5);
            tap(KEY_HOME);
        },
        [=, this] {
            if (utf8(edit->get_text()) != "sw") return "the edit box holds '" + utf8(edit->get_text()) + "', not 'sw'";
            if (dropped_delta().key == 0) return std::string("no key reached the policy; the unconsumed keys were not tested");
            return none("a key while typing");
        }});
    steps.push_back({"the pointer still reaches the world while typing", "UI-I3", [=, this] { motion(world_point); },
        [this] {
            if (viewport->gui_get_focus_owner() != edit) return std::string("the edit box lost focus");
            return world_delta().motion == 1 ? std::string{} : "world saw " + world_delta().text();
        }});
    steps.push_back({"keys return to the world when the edit box lets go", "UI-I3",
        [this] {
            edit->release_focus();
            tap(KEY_F5);
        },
        [this] { return world_delta().key == 2 ? std::string{} : "world saw " + world_delta().text(); }});
    steps.push_back({"a modal dialog blocks the world", "UI-I1",
        [=, this] {
            modal->show();
            click(MOUSE_BUTTON_RIGHT, world_point);
            button(MOUSE_BUTTON_WHEEL_UP, true, world_point);
            button(MOUSE_BUTTON_WHEEL_UP, false, world_point);
            tap(KEY_F5);
            tap(KEY_S, U's');
        },
        [=, this] {
            if (dropped_delta().key < 4) return "the policy dropped " + dropped_delta().text();
            return none("input under a modal dialog");
        }});
    steps.push_back({"closing the modal dialog returns input to the world", "UI-I1",
        [this] {
            click(MOUSE_BUTTON_LEFT, centre(*close));
            tap(KEY_F5);
        },
        [this] {
            if (modal->is_visible()) return std::string("the modal dialog is still open");
            const Counts d = world_delta();
            return d.key == 2 && d.press == 0 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"a world hold keeps the pointer over the HUD", "UI-I1",
        [=, this] {
            button(MOUSE_BUTTON_MIDDLE, true, clear);
            motion(opaque);
            button(MOUSE_BUTTON_MIDDLE, false, opaque);
        },
        [this] {
            const Counts d = world_delta();
            return d.press == 1 && d.motion == 1 && d.release == 1 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"without a hold the HUD takes pointer motion", "UI-I2",
        [=, this] {
            motion(opaque);
            motion(clear);
        },
        [this] { return world_delta().motion == 1 ? std::string{} : "world saw " + world_delta().text(); }});
    // #314 review P1: input the world holds when a modal opens must not outlive it. Presses and
    // the modal come in separate steps, as injected events arrive a frame after act().
    steps.push_back({"the world holds a key and a button", "UI-I1",
        [=, this] {
            button(MOUSE_BUTTON_MIDDLE, true, world_point);
            key(KEY_LEFT, 0, true);
        },
        [this] {
            const Counts d = world_delta();
            if (d.press != 1 || d.key != 1) return "world saw " + d.text();
            return held_keys.size() == 1 && held_buttons.size() == 1 ? std::string{} : "the world holds " + holds();
        }});
    steps.push_back({"opening a modal ends the world's holds", "UI-I1",
        [=, this] {
            modal->show();
            key(KEY_LEFT, 0, true, true);
            motion(world_point);
            motion(opaque);
            key(KEY_LEFT, 0, false);
            button(MOUSE_BUTTON_MIDDLE, false, world_point);
        },
        [this] {
            if (cancels_delta() != 1) return cancelled_text();
            if (!held_keys.empty() || !held_buttons.empty()) return "the world still holds " + holds();
            // The button's release reaches the world by policy (a release always does); the
            // world no longer holds the button, so it is inert.
            const Counts d = world_delta();
            return d.press == 0 && d.motion == 0 && d.key == 0 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"input pressed under the modal is inert", "UI-I1",
        [=, this] {
            key(KEY_RIGHT, 0, true);
            button(MOUSE_BUTTON_MIDDLE, true, world_point);
        },
        [=, this] {
            if (dropped_delta().key != 1) return "the policy dropped " + dropped_delta().text();
            return none("a press under the modal");
        }});
    steps.push_back({"closing the modal resurrects no hold", "UI-I1",
        [=, this] {
            modal->hide();
            key(KEY_RIGHT, 0, true, true);
            motion(world_point);
            key(KEY_RIGHT, 0, false);
            button(MOUSE_BUTTON_MIDDLE, false, world_point);
        },
        [this] {
            if (cancels_delta() != 1) return cancelled_text();
            if (!held_keys.empty() || !held_buttons.empty()) return "the world holds " + holds();
            const Counts d = world_delta();
            return d.press == 0 && d.motion == 1 ? std::string{} : "world saw " + d.text();
        }});
    steps.push_back({"HUD and world orders give equal commands", "UI-C1", [] {},
        [this]() -> std::string {
            if (!order_failures.empty()) return "an order failed: " + order_failures.front();
            if (commands.size() != 4) return std::to_string(commands.size()) + " commands, expected 4";
            const auto same = [&](const std::size_t a, const std::size_t b) {
                return commands[a].command.units == commands[b].command.units
                    && commands[a].command.payload == commands[b].command.payload;
            };
            if (!same(0, 2) || commands[0].origin != ui::CommandOrigin::hud_button
                || commands[2].origin != ui::CommandOrigin::hotkey) {
                return std::string("the HUD Stop button and the stop hotkey gave different commands");
            }
            if (!same(1, 3) || commands[1].origin != ui::CommandOrigin::world_click
                || commands[3].origin != ui::CommandOrigin::hud_button) {
                return std::string("the world right click and the HUD move mode gave different commands");
            }
            for (std::size_t index = 0; index < commands.size(); ++index) {
                const auto& key = commands[index].command.key;
                if (key.player_id != local_player || key.sequence != index) {
                    return std::string("commands are not stamped (tick, local player, next sequence)");
                }
            }
            return {};
        }});
}

void InputRoutingMode::State::drain() {
    auto taken = sink.scheduler.take(step);
    for (std::size_t index = 0; index < taken.size(); ++index) {
        commands.push_back({std::move(taken[index]),
            index < sink.origins.size() ? sink.origins[index] : ui::CommandOrigin::world_click});
    }
    sink.origins.clear();
}

bool InputRoutingMode::State::write_report(const bool passed) const {
    if (options.report_path.empty()) return true;
    std::ostringstream out;
    const auto quoted = [](const std::string& text) {
        std::string escaped = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') escaped += '\\';
            if (static_cast<unsigned char>(c) < 0x20U) continue;
            escaped += c;
        }
        return escaped + "\"";
    };
    out << "{\n  \"schema\": \"eawr-input-routing-v1\",\n  \"status\": " << quoted(passed ? "passed" : "failed")
        << ",\n  \"failure\": " << quoted(failure) << ",\n  \"backend\": {\"adapter_name\": "
        << quoted(utf8(RenderingServer::get_singleton()->get_video_adapter_name())) << "},\n  \"viewport\": ["
        << space.viewport.width << ", " << space.viewport.height << "],\n  \"steps\": [";
    for (std::size_t index = 0; index < results.size(); ++index) {
        const auto& result = results[index];
        out << (index == 0 ? "\n" : ",\n") << "    {\"name\": " << quoted(result.name) << ", \"rule\": "
            << quoted(result.rule) << ", \"passed\": " << (result.problem.empty() ? "true" : "false")
            << ", \"problem\": " << quoted(result.problem) << "}";
    }
    out << "\n  ],\n  \"commands\": [";
    for (std::size_t index = 0; index < commands.size(); ++index) {
        const auto& [command, origin] = commands[index];
        std::string verb = "stop";
        std::string detail;
        if (const auto* moved = std::get_if<tactical::MovePayload>(&command.payload)) {
            verb = "move";
            detail = ", \"destination_raw\": [" + std::to_string(moved->destination.x.raw()) + ", "
                + std::to_string(moved->destination.y.raw()) + ", " + std::to_string(moved->destination.z.raw()) + "]";
        } else if (const auto* attack = std::get_if<tactical::AttackPayload>(&command.payload)) {
            verb = "attack";
            detail = ", \"target\": " + std::to_string(attack->target);
        }
        out << (index == 0 ? "\n" : ",\n") << "    {\"tick\": " << command.key.tick << ", \"player\": "
            << command.key.player_id << ", \"sequence\": " << command.key.sequence << ", \"origin\": "
            << quoted(std::string(origin_name(origin))) << ", \"verb\": " << quoted(verb) << ", \"units\": [";
        for (std::size_t unit = 0; unit < command.units.size(); ++unit) {
            out << (unit == 0 ? "" : ", ") << command.units[unit];
        }
        out << "]" << detail << "}";
    }
    out << "\n  ],\n  \"world\": " << quoted(world.text()) << ",\n  \"dropped\": " << quoted(dropped.text())
        << ",\n  \"cancels\": " << cancels
        << "\n}\n";
    std::ofstream file(options.report_path, std::ios::binary | std::ios::trunc);
    file << out.str();
    return static_cast<bool>(file);
}

bool InputRoutingMode::ready(Node3D& host) {
    State& state = *state_;
    if (!state.build(host)) {
        static_cast<void>(state.write_report(false));
        return false;
    }
    return true;
}

std::optional<int> InputRoutingMode::process() {
    State& state = *state_;
    if (state.completed) return std::nullopt;
    if (--state.wait > 0) return std::nullopt;
    if (state.step < state.steps.size()) {
        Step& step = state.steps[state.step];
        if (state.acting) {
            state.before = state.world;
            state.dropped_before = state.dropped;
            state.cancels_before = state.cancels;
            step.act();
            state.acting = false;
            state.wait = frames_per_step;
            return std::nullopt;
        }
        state.drain();
        state.results.push_back({step.name, step.rule, step.check()});
        ++state.step;
        state.acting = true;
        state.wait = 1;
        return std::nullopt;
    }
    state.completed = true;
    bool passed = state.failure.empty();
    for (const auto& result : state.results) passed = passed && result.problem.empty();
    const bool written = state.write_report(passed);
    return passed && written ? 0 : 1;
}

bool InputRoutingMode::world_input(const Ref<InputEvent>& event) {
    State& state = *state_;
    std::uint32_t index{};
    const auto input = input_class(event, index);
    if (!input) return false;
    count(state.world, *input);
    if (const auto* key = Object::cast_to<InputEventKey>(event.ptr())) {
        const auto code = static_cast<std::int64_t>(key->get_keycode());
        if (!key->is_pressed()) {
            state.held_keys.erase(code);
        } else if (!key->is_echo()) {
            state.held_keys.insert(code);
        }
    } else if (*input == ui::InputClass::button_press) {
        state.held_buttons.insert(index);
    } else if (*input == ui::InputClass::button_release) {
        state.held_buttons.erase(index);
    }
    const auto point = [](const Vector2 position) {
        const auto whole = [](const float value) {
            return sim::math::Fixed::from_integer(static_cast<std::int64_t>(std::lround(value))).value();
        };
        return sim::math::Vec3{whole(position.x), whole(position.y), whole(0.0F)};
    };
    if (*input == ui::InputClass::button_press) {
        const auto* mouse = Object::cast_to<InputEventMouseButton>(event.ptr());
        const bool order = index == MOUSE_BUTTON_RIGHT
            || (index == MOUSE_BUTTON_LEFT && state.orders.mode() != ui::OrderMode::none);
        if (!order) return false;
        const auto origin = index == MOUSE_BUTTON_LEFT ? ui::CommandOrigin::hud_button : ui::CommandOrigin::world_click;
        auto issued = state.orders.world_command(
            ui::WorldPick{point(mouse->get_position()), sim::invalid_entity_id, false, false, false}, origin);
        if (!issued) state.order_failures.push_back(core::format_diagnostic(issued.error()));
        return true;
    }
    if (*input == ui::InputClass::key) {
        const auto* key = Object::cast_to<InputEventKey>(event.ptr());
        if (key->is_pressed() && !key->is_echo() && key->get_keycode() == KEY_S) {
            if (auto stopped = state.orders.stop(ui::CommandOrigin::hotkey); !stopped) {
                state.order_failures.push_back(core::format_diagnostic(stopped.error()));
            }
            return true;
        }
    }
    return false;
}

void InputRoutingMode::world_dropped(const ui::InputClass input) { count(state_->dropped, input); }

void InputRoutingMode::world_cancelled() {
    ++state_->cancels;
    state_->held_keys.clear();
    state_->held_buttons.clear();
}

} // namespace eawr::presentation::godot_backend
