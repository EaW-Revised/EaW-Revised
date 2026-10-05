#include "space_environment.hpp"
#include "space_environment_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {

[[nodiscard]] std::optional<float> parse_float(const std::string_view text) {
    if (text.empty()) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size() || !std::isfinite(value)) return std::nullopt;
    return value;
}


} // namespace

// meshgloss-light-probe: a labelled control, never the shipped sky policy. A
// constant per-channel irradiance (only the homogeneous term) makes Diffuse
// measurable, and an unnormalised light-zero direction along render -Z (the
// effect does not normalise it) makes Specular measurable on a surface whose
// normal faces render -Z. Values are invented test inputs.
[[nodiscard]] space::SkyLightPolicy space_environment_detail::light_probe_policy() {
    space::SkyLightPolicy policy;
    policy.id = "meshgloss-light-probe-control";
    policy.sph[0][15] = 0.5F;
    policy.sph[1][15] = 0.25F;
    policy.sph[2][15] = 0.125F;
    policy.light_direction = {0.0F, 0.0F, -3.0F};
    policy.light_specular = {1.0F, 1.0F, 1.0F};
    policy.light_scale = {1.0F, 1.0F, 1.0F, 1.0F};
    policy.cause = "labelled control only: invented constant irradiance (0.5, 0.25, 0.125) and light-zero specular "
                   "(1, 1, 1) along render -Z (0, 0, -3), in the render basis, to prove the Diffuse and Specular "
                   "bindings reach the GPU; not a space light rig";
    return policy;
}

[[nodiscard]] AdditiveControl space_environment_detail::parse_additive_control(const std::string_view control) {
    AdditiveControl result;
    std::vector<std::string_view> tokens;
    for (std::size_t start = 0; start < control.size();) {
        const std::size_t end = std::min(control.find(' ', start), control.size());
        if (end > start) tokens.push_back(control.substr(start, end - start));
        start = end + 1;
    }
    if (tokens.empty() || tokens.front() != meshadditive_control) return result;
    result.enabled = true;
    bool timed = false;
    bool scaled = false;
    for (std::size_t index = 1; index < tokens.size() && result.failure.empty(); ++index) {
        const std::string_view token = tokens[index];
        if (token == "occluder" && result.base == "none") {
            result.base = "occluder";
        } else if (token.starts_with("time=") && !timed) {
            timed = true;
            const auto value = parse_float(token.substr(5));
            if (!value) result.failure = "time= needs one finite number of seconds";
            else result.inputs.time = *value;
        } else if (token.starts_with("light-scale=") && !scaled) {
            scaled = true;
            std::array<float, 4> values{};
            std::size_t count = 0;
            bool valid = true;
            const std::string_view rest = token.substr(12);
            for (std::size_t first = 0; valid && first <= rest.size();) {
                const std::size_t comma = std::min(rest.find(',', first), rest.size());
                const auto value = parse_float(rest.substr(first, comma - first));
                if (!value || count == values.size()) valid = false;
                else values[count++] = *value;
                first = comma + 1;
            }
            if (!valid || count != values.size()) {
                result.failure = "light-scale= needs four comma-separated finite numbers";
            } else {
                result.inputs.light_scale = {values[0], values[1], values[2], values[3]};
            }
        } else {
            result.failure = "unknown or repeated token '" + std::string(token) + "'";
        }
    }
    if (timed || scaled) {
        result.inputs.id = "meshadditive-synthetic-inputs-control";
        result.inputs.cause = "labelled control only: invented TIME and LIGHT_SCALE test inputs that make the "
                              "UVScrollRate and light-scale bindings measurable; not the retail clock (G-04) or "
                              "sky light scale (G-05)";
    }
    if (result.failure.empty() && !space::finite_inputs(result.inputs)) result.failure = "inputs must be finite";
    return result;
}


} // namespace eawr::presentation::godot_backend
