#pragma once

// The environment's distance fog (R-DEC-08 and R-FOG-01..05 of docs/behaviour/p1-effective-environment.md): the record's
// fog colour, enable byte and start/end distances, the two shader constants derived from them, and the factor an effect's
// vertex stage computes from the eye distance. Engine-free; the renderer binds the constants as shared shader values.

#include "eawr/presentation/lighting/lighting.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace eawr::presentation::lighting {

inline constexpr std::uint32_t fog_color_field_id = 0x0fU;   // 3 x float
inline constexpr std::uint32_t fog_enabled_field_id = 0x10U; // byte
inline constexpr std::uint32_t fog_start_field_id = 0x11U;   // float
inline constexpr std::uint32_t fog_end_field_id = 0x12U;     // float

struct DistanceFog final {
    bool enabled{};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F}; // a stored (output) colour, as the fixed-function fog colour is
    float start_distance{1000.0F};
    float end_distance{10000.0F};
};

// The shader constants of one fog range: FOG_VALS = (1 / (near - far), -far / (near - far)), so that
// saturate(x * distance + y) is 1 at `near` and 0 at `far` (R-FOG-05). A range with near >= far cannot be expressed this
// way (the slope is not finite); it, and a disabled fog, give the neutral constants (0, 1): factor 1 at every distance.
struct FogVals final {
    float x{0.0F};
    float y{1.0F};
    friend bool operator==(const FogVals&, const FogVals&) = default;
};

[[nodiscard]] FogVals fog_vals(const DistanceFog& fog) noexcept;

// Both shared shader values for one scene's fog. No fog (nullopt), a disabled fog and an unusable range all give the
// neutral values: constants (0, 1) and white. A scene sets this every time it becomes current, so the values never carry
// over from an earlier scene.
struct FogGlobals final {
    FogVals vals{};
    std::array<float, 3> color{1.0F, 1.0F, 1.0F};
    friend bool operator==(const FogGlobals&, const FogGlobals&) = default;
};

[[nodiscard]] FogGlobals fog_globals(const std::optional<DistanceFog>& fog) noexcept;

// The unfogged fraction at an eye distance: saturate(x * distance + y).
[[nodiscard]] float fog_factor(const FogVals& vals, float distance) noexcept;

// The fixed-function result of blending a stored colour toward the fog colour: factor * colour + (1 - factor) * fog.
[[nodiscard]] std::array<float, 3> fogged_color(const std::array<float, 3>& color, const DistanceFog& fog,
                                                float distance) noexcept;

// Reads one TED environment record. The colour is a float3 at 0x0f (default white), the enable a byte at 0x10 (default
// off), start a float at 0x11 (default 1000) and end a float at 0x12 (default 10000); every occurrence is read and the last
// valid one wins (R-DEC-02); a scalar that is short or not finite keeps the previous value (R-DEC-04); a short or
// non-finite colour makes the whole record unreadable, which is nullopt here. Then start = min(start, 10000) and
// end = max(start, end) (R-DEC-08).
[[nodiscard]] std::optional<DistanceFog> distance_fog(std::span<const RawField> fields);

// Whether the retail vertex stage of an effect computes the fog factor from FOG_VALS (the effect is blended toward the fog
// colour) rather than writing a constant (additive and similar effects, which stay unfogged). `program` is the effect's
// name with or without its .fx / .fxo extension; ASCII case does not matter.
[[nodiscard]] bool program_fades_to_fog(std::string_view program) noexcept;

} // namespace eawr::presentation::lighting
