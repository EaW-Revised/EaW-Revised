#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The environment's distance fog (lighting/distance_fog.hpp) applied to a legacy adapter's Godot source. The retail
// vertex stage computes saturate(FOG_VALS.x * eye_distance + FOG_VALS.y) per vertex and the fixed-function fog blends the
// final colour toward the fog colour by it. This text step adds the same to a source that writes its final stored colour
// to ALBEDO: shared (global) shader values for the constants, the view-space position passed out of vertex(), and at the
// end of fragment() the factor from the per-pixel eye distance and the blend (project deviation: the factor is per pixel). With fog off the constants are (0, 1) and (1, 1, 1): the factor is exactly 1 and the blend returns
// ALBEDO unchanged (measured: stock maps without fog match within one colour level on at most a few pixels).
namespace eawr::presentation::godot_backend::distance_fog_source {

inline constexpr std::string_view vals_name = "eawr_distance_fog_vals";
inline constexpr std::string_view color_name = "eawr_distance_fog_color";

namespace detail {

// The position of the '}' that closes the function whose body opens at `open_brace`, or npos.
[[nodiscard]] inline std::size_t closing_brace(const std::string_view text, const std::size_t open_brace) noexcept {
    int depth = 0;
    for (std::size_t i = open_brace; i < text.size(); ++i) {
        if (text[i] == '{') ++depth;
        else if (text[i] == '}' && --depth == 0) return i;
    }
    return std::string_view::npos;
}

[[nodiscard]] inline std::size_t function_open_brace(const std::string_view text, const std::string_view signature) noexcept {
    const std::size_t at = text.find(signature);
    if (at == std::string_view::npos) return std::string_view::npos;
    return text.find('{', at + signature.size());
}

} // namespace detail

// The fogged source, or nullopt when the source is not eligible: it must be a spatial shader with a single fragment() that
// writes ALBEDO, and no additive/multiply blend (the fixed-function fog blends those toward black, which this step does
// not model) and not already fogged.
[[nodiscard]] inline std::optional<std::string> fogged(const std::string_view source) {
    constexpr std::string_view spatial = "shader_type spatial;";
    const std::size_t header = source.find(spatial);
    if (header == std::string_view::npos || source.find(vals_name) != std::string_view::npos) return std::nullopt;
    if (source.find("blend_add") != std::string_view::npos || source.find("blend_mul") != std::string_view::npos
        || source.find("blend_premul_alpha") != std::string_view::npos) return std::nullopt;
    const std::size_t fragment_open = detail::function_open_brace(source, "void fragment()");
    if (fragment_open == std::string_view::npos || source.find("ALBEDO", fragment_open) == std::string_view::npos) return std::nullopt;
    const std::size_t fragment_close = detail::closing_brace(source, fragment_open);
    if (fragment_close == std::string_view::npos) return std::nullopt;

    // The eye distance is taken per pixel from the interpolated view-space position: a factor interpolated between the
    // vertices of a few very large triangles (a sea plane) would be wrong everywhere between them.
    // The original measures the fog distance as the length of the projected (clip-space) xyz: x and y scaled by the
    // projection, z the depth. The view-space position is scaled the same way here, with z the view depth.
    const std::string factor_line = "    vec3 eawr_distance_fog_eye = (MODELVIEW_MATRIX * vec4(VERTEX, 1.0)).xyz;\n"
        "    eawr_distance_fog_view = vec3(abs(PROJECTION_MATRIX[0][0]) * eawr_distance_fog_eye.x,"
        " abs(PROJECTION_MATRIX[1][1]) * eawr_distance_fog_eye.y, eawr_distance_fog_eye.z);\n";
    const std::string blend_line = "    float eawr_distance_fog_factor = clamp(" + std::string(vals_name)
        + ".x * length(eawr_distance_fog_view) + " + std::string(vals_name) + ".y, 0.0, 1.0);\n    ALBEDO = mix("
        + std::string(color_name) + ", ALBEDO, eawr_distance_fog_factor);\n";
    const std::string declarations = "\nglobal uniform vec2 " + std::string(vals_name) + ";\nglobal uniform vec3 "
        + std::string(color_name) + ";\nvarying vec3 eawr_distance_fog_view;\n";

    std::string out;
    out.reserve(source.size() + 512);
    const std::size_t vertex_open = detail::function_open_brace(source, "void vertex()");
    const std::size_t vertex_close = vertex_open == std::string_view::npos ? std::string_view::npos
                                                                            : detail::closing_brace(source, vertex_open);
    if (vertex_open != std::string_view::npos && (vertex_close == std::string_view::npos || vertex_close > fragment_open)) {
        return std::nullopt; // a vertex() that is missing its end, or placed after fragment(), is not rewritten
    }
    // Edits in ascending position order: header, vertex end (or a new vertex function before fragment), fragment end.
    std::size_t cursor = 0;
    const auto copy_to = [&](const std::size_t end) { out.append(source.substr(cursor, end - cursor)); cursor = end; };
    copy_to(header + spatial.size());
    out += declarations;
    if (vertex_close != std::string_view::npos) {
        copy_to(vertex_close);
        out += factor_line;
    } else {
        const std::size_t fragment_start = source.rfind("void fragment()", fragment_open);
        copy_to(fragment_start);
        out += "void vertex() {\n" + factor_line + "}\n\n";
    }
    copy_to(fragment_close);
    out += blend_line;
    copy_to(source.size());
    return out;
}

} // namespace eawr::presentation::godot_backend::distance_fog_source
