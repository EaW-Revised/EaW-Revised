#pragma once

#include <string_view>

namespace eawr::viewer::galactic {

// --eawr-galactic-ai on|off: native galactic AI is ON by default (release default, owner 10-17).
// Only "off" switches it off; anything other than on/off is flagged invalid.
struct NativeAiFlag final {
    bool enabled{true};
    bool invalid{};
};

inline void apply_native_ai_flag(NativeAiFlag& flag, std::string_view value) noexcept {
    flag.enabled = value == "on";
    flag.invalid |= value != "on" && value != "off";
}

} // namespace eawr::viewer::galactic
