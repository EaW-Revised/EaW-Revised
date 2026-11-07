#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

namespace eawr::presentation::godot_backend::detail {

template <typename ApplySettings>
void apply_shadow_settings_if_enabled(const bool enabled, ApplySettings&& apply_settings) {
    if (enabled) apply_settings();
}

// The directional shadow filter belongs to the RenderingServer, not a scene.
// Serialize the check with the setter so multiple renderer instances share it.
class GlobalShadowQuality final {
public:
    template <typename SetQuality>
    void apply(const std::uint64_t server, const int quality, SetQuality&& set_quality) {
        const std::lock_guard lock(mutex_);
        if (server_ == server && quality_ == quality) return;
        set_quality();
        server_ = server;
        quality_ = quality;
    }

private:
    std::mutex mutex_;
    std::optional<std::uint64_t> server_;
    std::optional<int> quality_;
};

} // namespace eawr::presentation::godot_backend::detail
