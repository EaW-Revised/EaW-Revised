#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::godot_backend {

// Whether the initial compose of a live session decodes its textures ahead on reader threads (scene::TexturePrefetch, which
// also builds the mip chains of #5989), and with how many readers. Presentation only: it changes when a texture is decoded,
// never what the simulation sees. With the read-ahead on, a texture it fails to decode or has not started is decoded and
// chained by the loader instead, the same result built on the compose's own thread (a longer load, nothing else); a
// texture comes out plain only when there is no read-ahead (Safe render thread, the off override, an arrival).
//
// On for every live session (a skirmish, a replay, the attract demo: the choice has no attract input, so it cannot go off
// for a real battle by being wired for the demo only) on the Separate render thread; the Safe render thread has none.
// The user arguments `--eawr-texture-prefetch off|on` and `--eawr-texture-readers <n>` override it (`on` never beats Safe).
struct TexturePrefetchChoice final {
    bool enabled{};
    unsigned readers{}; // nonzero fixes the reader count (A/B, tests); zero lets the disk kind decide
};

[[nodiscard]] inline TexturePrefetchChoice choose_texture_prefetch(const bool separate_render_thread,
                                                                   const std::span<const std::string> user_args) {
    TexturePrefetchChoice choice;
    choice.enabled = separate_render_thread;
    for (std::size_t i = 0; i + 1 < user_args.size(); ++i) {
        const std::string_view name = user_args[i];
        const std::string_view value = user_args[i + 1];
        if (name == "--eawr-texture-prefetch") choice.enabled = separate_render_thread && value != "off";
        if (name == "--eawr-texture-readers") {
            const long parsed = std::strtol(std::string(value).c_str(), nullptr, 10);
            choice.readers = static_cast<unsigned>(std::max(0L, parsed));
        }
    }
    return choice;
}

// The space population runs the read-ahead for a live session only; a static --eawr-populate map keeps its plain textures.
[[nodiscard]] constexpr bool population_uses_texture_prefetch(const bool live_session, const bool enabled) noexcept {
    return live_session && enabled;
}

} // namespace eawr::presentation::godot_backend
