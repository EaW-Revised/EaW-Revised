#pragma once

#include <string_view>

namespace eawr::viewer::galactic {

// --eawr-galactic-ai on|off: native galactic AI is OFF by default in 0.2.5 (saves taken while a
// battle prompt is pending cannot be loaded with AI on, #4068). Anything other than on/off is invalid.
struct NativeAiFlag final {
    bool enabled{};
    bool invalid{};
};

inline void apply_native_ai_flag(NativeAiFlag& flag, std::string_view value) noexcept {
    flag.enabled = value == "on";
    flag.invalid |= value != "on" && value != "off";
}

} // namespace eawr::viewer::galactic
