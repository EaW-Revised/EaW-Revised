#include "startup_window.hpp"

#ifdef _WIN32
#include <godot_cpp/classes/display_server.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/thread.hpp>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#endif

namespace eawr::presentation::godot_backend {
namespace {
#ifdef _WIN32
thread_local bool dispatch_startup_messages{};
#endif
}

bool startup_window_available() {
#ifdef _WIN32
    // Guard before even peeking: each thread has its own native message queue.
    if (!godot::Thread::is_main_thread()) return false;
    // DisplayServer::get_singleton() has no null check in godot-cpp release builds: before the engine
    // creates the display server it would dereference null. Ask the engine first.
    auto* engine = godot::Engine::get_singleton();
    if (!engine || !engine->has_singleton("DisplayServer")) return false;
    const auto* display = godot::DisplayServer::get_singleton();
    return display && display->get_name() != godot::String("headless");
#else
    return false;
#endif
}

void pump_startup_messages() {
#ifdef _WIN32
    if (!startup_window_available()) return;
    MSG message{};
    PeekMessageW(&message, nullptr, 0, 0, PM_NOREMOVE);
    if (!dispatch_startup_messages) return;
    godot::DisplayServer::get_singleton()->process_events();
    if (auto* input = godot::Input::get_singleton()) input->flush_buffered_events();
#endif
}

StartupFrameMessages::StartupFrameMessages() {
#ifdef _WIN32
    previous_ = dispatch_startup_messages;
    if (startup_window_available()) dispatch_startup_messages = true;
#endif
}
StartupFrameMessages::~StartupFrameMessages() {
#ifdef _WIN32
    dispatch_startup_messages = previous_;
#endif
}
} // namespace eawr::presentation::godot_backend
