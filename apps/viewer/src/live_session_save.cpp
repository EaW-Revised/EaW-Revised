// #3989: a player's save of a local skirmish battle (docs/checkpoint-format.md "Skirmish battle saves").
//
// Threads, in order:
//  - frame thread: save_game() asks the battle's feed for a save (LiveSession::request_save);
//  - simulation thread: takes the battle between two ticks (also while paused) and hands the bytes to
//    the sink below, which only stores them in the PendingSave (its own mutex);
//  - frame thread: service_save() takes the stored bytes and starts the disk worker;
//  - disk worker (std::async): adds the view's hints and writes <directory>/<name>.eawrsave
//    (platform::write_battle_save); service_save() reads its result on the frame thread.
// The view's members are touched on the frame thread only.

#include "live_session_view.hpp"

#include "eawr/platform/saved_games.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

#include <chrono>
#include <mutex>
#include <utility>

namespace eawr::presentation::godot_backend {

struct LiveSessionView::PendingSave final {
    std::filesystem::path directory;
    std::string name;
    bool overwrite{};
    std::vector<std::uint8_t> hints;
    std::mutex mutex; // the sink (simulation thread) and service_save (frame thread)
    std::optional<std::pair<std::uint64_t, core::Result<std::vector<std::uint8_t>>>> taken;
};

bool LiveSessionView::can_save() const noexcept {
    return session_ != nullptr && setup_.has_value() && !setup_->campaign && ai_scripts_ != nullptr
        && !options_.lan && !options_.borrowed_feed && !replay_ && !land_content_ && !observer_multiplayer()
        && phase_ == Phase::running;
}

bool LiveSessionView::save_game(std::filesystem::path directory, std::string name, const bool overwrite,
                                std::vector<std::uint8_t> hints) {
    if (!can_save() || save_status_.running) return false;
    auto pending = std::make_shared<PendingSave>();
    pending->directory = std::move(directory);
    pending->name = std::move(name);
    pending->overwrite = overwrite;
    pending->hints = std::move(hints);
    const bool requested = session_->request_save(
        [pending](const std::uint64_t tick, core::Result<std::vector<std::uint8_t>> bytes) {
            const std::lock_guard lock(pending->mutex);
            pending->taken.emplace(tick, std::move(bytes));
        });
    if (!requested) return false;
    pending_save_ = std::move(pending);
    save_status_.running = true;
    return true;
}

bool LiveSessionView::service_save() {
    if (pending_save_) {
        std::optional<std::pair<std::uint64_t, core::Result<std::vector<std::uint8_t>>>> taken;
        {
            const std::lock_guard lock(pending_save_->mutex);
            taken = std::exchange(pending_save_->taken, std::nullopt);
        }
        if (taken) {
            auto pending = std::exchange(pending_save_, nullptr);
            save_status_.last_name = pending->name;
            save_status_.last_tick = taken->first;
            if (!taken->second) {
                save_status_.running = false;
                ++save_status_.failed;
                save_status_.last_problem = taken->second.error().message;
                godot::UtilityFunctions::print("EAWR battle save failed: ", godot::String::utf8(save_status_.last_problem.c_str()));
                return true;
            }
            save_write_ = std::async(std::launch::async,
                [pending, bytes = std::move(taken->second).value()] {
                    return platform::write_battle_save(pending->directory, pending->name, bytes, pending->hints,
                                                       pending->overwrite);
                });
        }
    }
    if (!save_write_.valid() || save_write_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return false;
    const auto written = save_write_.get();
    save_status_.running = false;
    if (written) {
        ++save_status_.written;
        save_status_.last_problem.clear();
        godot::UtilityFunctions::print("EAWR battle saved: ", godot::String::utf8(save_status_.last_name.c_str()), " at tick ",
                                static_cast<int64_t>(save_status_.last_tick));
    } else {
        ++save_status_.failed;
        save_status_.last_problem = written.error().message;
        godot::UtilityFunctions::print("EAWR battle save failed: ", godot::String::utf8(save_status_.last_problem.c_str()));
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
