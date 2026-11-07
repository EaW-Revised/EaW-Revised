#pragma once

#include <optional>
#include <utility>

namespace eawr::presentation::godot_backend::detail {

// L1.2: a value main snapshots for a lane thread, with the live source as the fallback. While a latch is set the
// reader never calls `live`, so a thread that must not touch the live source (the engine's process-frame counter,
// the main thread's thread_local camera map) cannot reach it. An unset latch reads live: the inline path.
template<class Value>
class ParticleLatch final {
public:
    void set(std::optional<Value> value) noexcept { value_ = std::move(value); }
    [[nodiscard]] bool latched() const noexcept { return value_.has_value(); }
    template<class Live>
    [[nodiscard]] Value read(Live&& live) const {
        if (value_) return *value_;
        return std::forward<Live>(live)();
    }

private:
    std::optional<Value> value_;
};

} // namespace eawr::presentation::godot_backend::detail
