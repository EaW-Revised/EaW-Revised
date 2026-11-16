#pragma once

#include <utility>
#ifdef _WIN32
#include "eawr/core/load_profile.hpp"
#include <chrono>
#include <future>
#include <optional>
#endif

namespace eawr::presentation::godot_backend {

// Before the first frame, only observe the queue: dispatching a caption press
// can enter the native move loop and start frames before SceneTree is ready.
void pump_startup_messages();
[[nodiscard]] bool startup_window_available();

// Only the dedicated startup branch inside ViewerHost::_process owns this
// scope. Main::iteration is already running there, preventing modal re-entry.
class StartupFrameMessages final {
public:
    StartupFrameMessages();
    ~StartupFrameMessages();
    StartupFrameMessages(const StartupFrameMessages&) = delete;
    StartupFrameMessages& operator=(const StartupFrameMessages&) = delete;
private:
    bool previous_{};
};

#ifdef _WIN32
namespace startup_window_detail {
struct ReadProfile final {
    core::load_profile::Recording* target{core::load_profile::active};
    std::optional<core::load_profile::Recording> worker;
    ReadProfile() {
        if (target) {
            worker.emplace();
            worker->detailed = target->detailed;
        }
    }
    // Declared before the future, so even an exception joins the worker before
    // its recording is merged back into the main thread's timeline.
    ~ReadProfile() { if (target) core::load_profile::merge(*target, *worker); }
};
struct BindProfile final {
    core::load_profile::Recording* previous{core::load_profile::active};
    explicit BindProfile(core::load_profile::Recording* recording) { core::load_profile::active = recording; }
    ~BindProfile() { core::load_profile::active = previous; }
};
} // namespace startup_window_detail
#endif

// Only engine-free reads belong in this callable. The owning thread checks
// messages while waiting; dispatch is enabled only inside a startup frame.
// get() joins before returning and propagates the original load exception.
template<class Read>
auto read_with_startup_messages(Read&& read) {
#ifdef _WIN32
    if (startup_window_available()) {
        startup_window_detail::ReadProfile profile;
        auto pending = std::async(std::launch::async, [&profile, read = std::forward<Read>(read)]() mutable {
            startup_window_detail::BindProfile binding(profile.worker ? &*profile.worker : nullptr);
            return read();
        });
        while (pending.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
            pump_startup_messages();
        }
        return pending.get();
    }
#endif
    return std::forward<Read>(read)();
}

} // namespace eawr::presentation::godot_backend
