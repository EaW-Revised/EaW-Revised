#include "shader_adapter.hpp"

#include <array>
#include <cmath>
#include <cstdlib>
#include <string_view>

namespace {

bool close(const float left, const float right, const float tolerance = 1.0e-5F) {
    return std::abs(left - right) <= tolerance;
}

} // namespace

int main() {
    using namespace eawr::godot_prototype;

    SphChannelMatrix discriminating;
    discriminating.columns[0][3] = 0.2F;
    discriminating.columns[3][0] = 0.2F;
    if (!close(evaluate_quadratic(discriminating, {1.0F, 0.0F, 0.0F, 1.0F}), 0.4F)
        || !close(evaluate_quadratic(discriminating, {-1.0F, 0.0F, 0.0F, 1.0F}), -0.4F)) {
        return EXIT_FAILURE;
    }

    const std::array<float, 3> light_raw = {0.35F, 0.75F, 0.56F};
    const float length = std::sqrt(light_raw[0] * light_raw[0]
        + light_raw[1] * light_raw[1] + light_raw[2] * light_raw[2]);
    const std::array<float, 3> light = {
        light_raw[0] / length, light_raw[1] / length, light_raw[2] / length};
    const std::array<float, 4> aligned = {light[0], light[1], light[2], 1.0F};
    const std::array<float, 4> opposed = {-light[0], -light[1], -light[2], 1.0F};
    const auto red = meshgloss_hemisphere_matrix(0.08F, 2.0F, light);
    if (!close(evaluate_quadratic(red, aligned), 2.08F)
        || !close(evaluate_quadratic(red, opposed), 0.08F)) {
        return EXIT_FAILURE;
    }

    if (meshgloss_uses_alpha_blend(1.0F) || !meshgloss_uses_alpha_blend(0.5F)) {
        return EXIT_FAILURE;
    }

    // Clean behavior note cases 3 and 4: exponent 16 and gloss-in-texture-alpha.
    if (!close(std::pow(0.5F, 16.0F), 1.0F / 65536.0F)) return EXIT_FAILURE;
    const std::array<float, 3> vertex_diffuse = {0.2F, 0.2F, 0.45F};
    const std::array<float, 3> vertex_specular = {0.6F, 0.2F, 0.05F};
    const std::array<float, 3> sampled_rgb = {0.25F, 0.5F, 1.0F};
    const auto fragment_channel = [&](const std::size_t channel, const float texture_alpha) {
        return 2.0F * vertex_diffuse[channel] * sampled_rgb[channel]
            + vertex_specular[channel] * texture_alpha;
    };
    if (!close(fragment_channel(0, 0.4F), 0.34F)
        || !close(fragment_channel(1, 0.4F), 0.28F)
        || !close(fragment_channel(2, 0.4F), 0.92F)
        || !close(fragment_channel(0, 0.0F), 0.10F)) {
        return EXIT_FAILURE;
    }

    // The frozen scene's clear colour is linear.  Its red channel must cross
    // the Compatibility renderer's sRGB-valued output API as about 18/255,
    // not remain the prior incorrect 1-2/255 linear byte value.
    if (!close(linear_to_srgb_component(0.006F), 0.0701648F, 1.0e-5F)
        || std::lround(linear_to_srgb_component(0.006F) * 255.0F) != 18
        || !close(linear_to_srgb_component(0.5F), 0.735357F, 1.0e-5F)
        || !close(srgb_to_linear_component(linear_to_srgb_component(0.5F)), 0.5F)) {
        return EXIT_FAILURE;
    }

    const auto shader = meshgloss_shader_opaque;
    if (shader.find("pow(max(dot(normal_world, half_direction), 0.0), 16.0)") == std::string_view::npos
        || shader.find("eawr_vertex_specular * base_sample.a") == std::string_view::npos
        || shader.find("eawr_srgb_to_linear(base_sample.rgb)") == std::string_view::npos
        || shader.find("eawr_linear_to_srgb(eawr_linear_rgb)") == std::string_view::npos
        || shader.find("roughness") != std::string_view::npos
        || shader.find("metallic") != std::string_view::npos) {
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
