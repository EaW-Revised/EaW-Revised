#include "fog_renderer_probe.hpp"

void EawrFogRendererProbe::expect_forced_unbound(const sim::AssetId asset, const std::string& label) {
    // Explicit overrides, whatever the shader default or a pre-existing
    // override: bound false, no texture override, neutral mapping.
    const auto parameters = fog_parameters(asset);
    const auto value = [&parameters](const std::string_view name) {
        for (const auto& [key, text] : parameters) if (key == name) return text;
        return std::string("missing");
    };
    check(value("eawr_fog_bound") == "bool:false", label + ": asset " + std::to_string(asset)
        + " eawr_fog_bound is " + value("eawr_fog_bound") + ", not a forced false override");
    check(value("eawr_fog_texture") == "Nil:<null>", label + ": asset " + std::to_string(asset)
        + " still refers to a texture (" + value("eawr_fog_texture") + ")");
    for (const auto& [name, neutral] : {std::pair<std::string_view, std::string_view>{"eawr_fog_origin", "Vector2:(0.0, 0.0)"},
             {"eawr_fog_extent", "Vector2:(1.0, 1.0)"}, {"eawr_fog_size", "Vector2:(1.0, 1.0)"}}) {
        check(value(name) == neutral, label + ": asset " + std::to_string(asset) + " " + std::string(name)
            + " is " + value(name) + ", not the neutral override " + std::string(neutral));
    }
}
void EawrFogRendererProbe::verify(const Step& step, const PackedByteArray& pixels) {
    if (step.capture == Capture::alpha_background) {
        alpha_background_ = pixels;
        return;
    }
    if (step.capture == Capture::alpha_control) {
        alpha_controls_[step.control_byte] = pixels;
        return;
    }
    if (step.capture == Capture::alpha_compare) {
        const auto control = alpha_controls_.find(step.control_byte);
        check(control != alpha_controls_.end(), step.name + ": missing same-pipeline alpha control");
        if (control == alpha_controls_.end()) return;
        int max_error = 0;
        int compared = 0;
        std::array<int, 3> mean{};
        std::array<int, 3> background{};
        // The centre of the transformed plate is inside the asymmetric grid,
        // away from texel boundaries and against the nonblack opaque terrain.
        for (int y = view_height / 2 - 4; y <= view_height / 2 + 4; ++y) {
            for (int x = view_width / 2 - 4; x <= view_width / 2 + 4; ++x) {
                const std::int64_t offset = (static_cast<std::int64_t>(y) * view_width + x) * 4;
                for (int channel = 0; channel < 3; ++channel) {
                    max_error = std::max(max_error, std::abs(
                        static_cast<int>(pixels[offset + channel])
                        - static_cast<int>(control->second[offset + channel])));
                    mean[channel] += pixels[offset + channel];
                    background[channel] += alpha_background_[offset + channel];
                }
                ++compared;
            }
        }
        check(max_error <= 2, step.name + ": alpha composite differs from same-pipeline linear-RGB control by "
            + std::to_string(max_error));
        if (step.control_byte == 0) {
            check(background[0] - mean[0] > 2 * compared,
                step.name + ": zero visibility erased source alpha against nonblack terrain");
        }
        renders_.push_back("{\"stage\":\"" + escape(step.name)
            + "\",\"kind\":\"alpha-control-comparison\",\"byte\":" + std::to_string(step.control_byte)
            + ",\"compared\":" + std::to_string(compared) + ",\"max_error\":"
            + std::to_string(max_error) + ",\"background_rgb\":["
            + std::to_string(background[0] / compared) + "," + std::to_string(background[1] / compared)
            + "," + std::to_string(background[2] / compared) + "],\"rgb\":["
            + std::to_string(mean[0] / compared) + "," + std::to_string(mean[1] / compared)
            + "," + std::to_string(mean[2] / compared) + "]}");
        return;
    }
    if (step.capture == Capture::alpha_spatial) {
        struct Sample { double x; double y; std::uint8_t byte; };
        constexpr std::array<Sample, 6> samples{{
            {0.0, 0.0, 0}, {0.0, 0.6, 60}, {2.0, 0.6, 110},
            {0.0, 1.4, 210}, {2.0, 1.4, 255}, {0.0, 1.95, 0},
        }};
        const double tan_half = std::tan(fov_degrees * 0.5 * 3.14159265358979323846 / 180.0);
        const double depth = camera_y - 0.5;
        int max_error = 0;
        std::string result = "[";
        for (const Sample& sample : samples) {
            const auto reference = alpha_controls_.find(sample.byte);
            check(reference != alpha_controls_.end(), step.name + ": missing alpha spatial control");
            if (reference == alpha_controls_.end()) return;
            const int px = static_cast<int>(std::lround(view_width * 0.5
                + (sample.x - camera_x) * view_height / (2.0 * depth * tan_half)));
            const int py = static_cast<int>(std::lround(view_height * 0.5
                - (sample.y + camera_z) * view_height / (2.0 * depth * tan_half)));
            for (int y = py - 1; y <= py + 1; ++y) {
                for (int x = px - 1; x <= px + 1; ++x) {
                    const std::int64_t offset = (static_cast<std::int64_t>(y) * view_width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel) {
                        max_error = std::max(max_error, std::abs(
                            static_cast<int>(pixels[offset + channel])
                            - static_cast<int>(reference->second[offset + channel])));
                    }
                }
            }
            if (result.size() > 1) result += ",";
            result += "{\"x\":" + std::to_string(sample.x) + ",\"y\":" + std::to_string(sample.y)
                + ",\"byte\":" + std::to_string(sample.byte) + "}";
        }
        result += "]";
        check(max_error <= 2, step.name + ": asymmetric mapping or half-open bounds differ from controls by "
            + std::to_string(max_error));
        renders_.push_back("{\"stage\":\"" + escape(step.name)
            + "\",\"kind\":\"alpha-spatial-comparison\",\"max_error\":"
            + std::to_string(max_error) + ",\"samples\":" + result + "}");
        return;
    }
    if (step.capture == Capture::control) {
        controls_[step.control_byte] = pixels;
        return;
    }
    if (step.capture == Capture::baseline) {
        baseline_ = pixels;
        return;
    }
    if (step.capture == Capture::raw) {
        // Pixel-exact equality with the never-enabled baseline.
        int differences = 0;
        const std::int64_t count = std::min(pixels.size(), baseline_.size());
        for (std::int64_t i = 0; i < count; ++i) differences += pixels[i] != baseline_[i] ? 1 : 0;
        check(pixels.size() == baseline_.size() && differences == 0,
            step.name + ": " + std::to_string(differences) + " bytes differ from the never-enabled baseline");
        renders_.push_back("{\"stage\":\"" + escape(step.name) + "\",\"kind\":\"baseline-equality\",\"differing_bytes\":"
            + std::to_string(differences) + ",\"compared_bytes\":" + std::to_string(count) + "}");
        return;
    }
    if (step.capture == Capture::keep || step.capture == Capture::kept) {
        // keep: the pre-fog render of the override control, which must differ
        // visibly from the never-enabled baseline (its overrides draw a lit
        // window). kept: pixel-exact equality with that pre-fog render.
        const PackedByteArray& reference = step.capture == Capture::keep ? baseline_ : kept_;
        int differences = 0;
        const std::int64_t count = std::min(pixels.size(), reference.size());
        for (std::int64_t i = 0; i < count; ++i) differences += pixels[i] != reference[i] ? 1 : 0;
        if (step.capture == Capture::keep) {
            kept_ = pixels;
            check(differences > 10000, step.name + ": pre-existing overrides are not visible (" + std::to_string(differences)
                + " bytes differ from the baseline)");
        } else {
            check(pixels.size() == reference.size() && differences == 0,
                step.name + ": " + std::to_string(differences) + " bytes differ from the pre-fog render");
        }
        renders_.push_back("{\"stage\":\"" + escape(step.name) + "\",\"kind\":\""
            + (step.capture == Capture::keep ? "override-visible" : "pre-fog-equality") + "\",\"differing_bytes\":"
            + std::to_string(differences) + ",\"compared_bytes\":" + std::to_string(count) + "}");
        return;
    }

    const sim_fog::FogGrid* grid = step.expected ? step.expected() : nullptr;
    const double tan_half = std::tan(fov_degrees * 0.5 * 3.14159265358979323846 / 180.0);
    const double aspect = static_cast<double>(view_width) / view_height;
    int compared = 0;
    int compared_terrain = 0;
    int compared_unit = 0;
    int compared_outside = 0;
    int skipped = 0;
    int mismatches = 0;
    int max_error = 0;
    std::string first_mismatch;
    // Per surface, per fog byte: summed fog RGB, pixel count, and summed
    // unattenuated (byte-255 control) RGB over the same pixels.
    std::map<std::pair<int, int>, std::array<double, 7>> sums;
    for (int py = 0; py < view_height; ++py) {
        for (int px = 0; px < view_width; ++px) {
            const double ndc_x = (px + 0.5) / view_width * 2.0 - 1.0;
            const double ndc_y = 1.0 - (py + 0.5) / view_height * 2.0;
            const Vector3 eye(static_cast<real_t>(camera_x), static_cast<real_t>(camera_y), static_cast<real_t>(camera_z));
            const Vector3 direction(static_cast<real_t>(ndc_x * tan_half * aspect), -1.0F,
                static_cast<real_t>(-ndc_y * tan_half));
            int hit_surface = -1;
            bool ambiguous = false;
            Vector3 hit;
            double pixel_size = 0.0;
            for (std::size_t s = 0; s < surfaces_.size() && hit_surface < 0 && !ambiguous; ++s) {
                const Surface& surface = surfaces_[s];
                const Vector3 normal = surface.transform.basis.xform(Vector3(0, 1, 0)).normalized();
                const double denominator = normal.dot(direction);
                if (std::abs(denominator) < 1e-9) continue;
                const double t = normal.dot(surface.transform.origin - eye) / denominator;
                const Vector3 point = eye + direction * static_cast<real_t>(t);
                const Vector3 local = surface.inverse.xform(point);
                const double ax = local.x;
                const double ay = -local.z;
                pixel_size = 2.0 * (camera_y - point.y) * tan_half / view_height;
                const double margin = 1.5 * pixel_size;
                const bool inside = std::abs(ax) < surface.half_x - margin && std::abs(ay) < surface.half_y - margin;
                const bool outside = std::abs(ax) > surface.half_x + margin || std::abs(ay) > surface.half_y + margin;
                if (inside) {
                    hit_surface = static_cast<int>(s);
                    hit = point;
                } else if (!outside) {
                    ambiguous = true;
                }
            }
            if (hit_surface < 0) {
                ++skipped;
                continue;
            }
            const fog::SourcePoint source = fog::source_from_render(hit.x, hit.y, hit.z);
            std::uint8_t byte = 0;
            if (grid != nullptr) {
                const auto mapping = fog::mapping_for(grid->desc());
                const double cell_w = mapping.extent_x / mapping.width;
                const double cell_h = mapping.extent_y / mapping.height;
                const double fx = (source.x - mapping.origin_x) / cell_w;
                const double fy = (source.y - mapping.origin_y) / cell_h;
                const double line_x = std::clamp(std::round(fx), 0.0, static_cast<double>(mapping.width));
                const double line_y = std::clamp(std::round(fy), 0.0, static_cast<double>(mapping.height));
                const bool near_x = std::abs(fx - line_x) * cell_w < 1.5 * pixel_size
                    && fy > -1.5 * pixel_size / cell_h && fy < mapping.height + 1.5 * pixel_size / cell_h;
                const bool near_y = std::abs(fy - line_y) * cell_h < 1.5 * pixel_size
                    && fx > -1.5 * pixel_size / cell_w && fx < mapping.width + 1.5 * pixel_size / cell_w;
                if (near_x || near_y) {
                    ++skipped;
                    continue;
                }
                byte = fog::attenuation_at(*grid, source);
                if (!fog::cell_at_raw(grid->desc(), std::llround(source.x * one), std::llround(source.y * one))) {
                    ++compared_outside;
                }
            }
            const auto control = controls_.find(byte);
            if (control == controls_.end()) {
                check(false, step.name + ": no control for byte " + std::to_string(byte));
                return;
            }
            const std::int64_t offset = (static_cast<std::int64_t>(py) * view_width + px) * 4;
            int error = 0;
            for (int c = 0; c < 3; ++c) {
                error = std::max(error, std::abs(static_cast<int>(pixels[offset + c]) - static_cast<int>(control->second[offset + c])));
            }
            auto& sum = sums[{hit_surface, byte}];
            const PackedByteArray& unattenuated = controls_.at(255);
            for (int c = 0; c < 3; ++c) {
                sum[static_cast<std::size_t>(c)] += pixels[offset + c];
                sum[static_cast<std::size_t>(c) + 4] += unattenuated[offset + c];
            }
            sum[3] += 1.0;
            max_error = std::max(max_error, error);
            ++compared;
            (hit_surface == 0 && surfaces_.size() == 2 ? compared_unit : compared_terrain) += 1;
            if (error > 1) {
                if (mismatches == 0) {
                    first_mismatch = "pixel " + std::to_string(px) + "," + std::to_string(py) + " rgb "
                        + std::to_string(pixels[offset]) + "," + std::to_string(pixels[offset + 1]) + ","
                        + std::to_string(pixels[offset + 2]) + " control(" + std::to_string(byte) + ") "
                        + std::to_string(control->second[offset]) + "," + std::to_string(control->second[offset + 1])
                        + "," + std::to_string(control->second[offset + 2]);
                }
                ++mismatches;
            }
        }
    }
    check(mismatches == 0, step.name + ": " + std::to_string(mismatches) + " pixel mismatches; first " + first_mismatch);
    check(compared_terrain > 20000 && compared_unit > 3000, step.name + ": too few compared pixels ("
        + std::to_string(compared_terrain) + " terrain, " + std::to_string(compared_unit) + " unit)");

    // Visible attenuation: per (surface, byte), the mean fog RGB against the
    // unattenuated same-pipeline control over the very same pixels. Byte 255
    // must equal it; every other byte must be darker on every channel.
    std::string means = "[";
    std::map<int, int> attenuated_bytes;
    std::map<int, bool> has_full;
    for (const auto& [key, sum] : sums) {
        const std::string surface = key.first == 0 && surfaces_.size() == 2 ? "unit" : "terrain";
        std::array<double, 3> fogged{};
        std::array<double, 3> full{};
        for (std::size_t c = 0; c < 3; ++c) {
            fogged[c] = sum[c] / sum[3];
            full[c] = sum[c + 4] / sum[3];
        }
        if (grid != nullptr && sum[3] >= 50) {
            if (key.second == 255) {
                has_full[key.first] = true;
                for (std::size_t c = 0; c < 3; ++c) {
                    check(std::abs(fogged[c] - full[c]) <= 1.0, step.name + ": " + surface + " byte 255 is not unattenuated");
                }
            } else {
                double difference = 0.0;
                bool darker = true;
                for (std::size_t c = 0; c < 3; ++c) {
                    difference += full[c] - fogged[c];
                    darker = darker && (fogged[c] < full[c] || full[c] < 1.0);
                }
                check(darker && difference >= 6.0, step.name + ": " + surface + " byte " + std::to_string(key.second)
                    + " is not visibly attenuated");
                if (key.second != 0) ++attenuated_bytes[key.first];
            }
        }
        if (means.size() > 1) means += ",";
        means += "{\"surface\":\"" + surface + "\",\"byte\":" + std::to_string(key.second)
            + ",\"pixels\":" + std::to_string(static_cast<int>(sum[3]))
            + ",\"rgb\":[" + std::to_string(std::lround(fogged[0])) + "," + std::to_string(std::lround(fogged[1]))
            + "," + std::to_string(std::lround(fogged[2])) + "],\"unattenuated_rgb\":["
            + std::to_string(std::lround(full[0])) + "," + std::to_string(std::lround(full[1])) + ","
            + std::to_string(std::lround(full[2])) + "]}";
    }
    means += "]";
    // Coverage is demanded where the grid layout guarantees it: at the first
    // (and lit) grid both paths show at least three attenuated cells and a full one.
    if (grid != nullptr && (step.name == "first-grid" || step.name == "lit-fog")) {
        for (int surface = 0; surface < 2; ++surface) {
            const std::string label = surface == 0 ? "unit" : "terrain";
            check(attenuated_bytes[surface] >= 3 && has_full[surface],
                step.name + ": " + label + " shows fewer than three attenuated bytes or no unattenuated cell");
        }
    }
    renders_.push_back("{\"stage\":\"" + escape(step.name) + "\",\"kind\":\"control-comparison\",\"expected\":\""
        + (grid == nullptr ? std::string("dark") : "team " + std::to_string(grid->team_id()) + " revision "
            + std::to_string(grid->revision()))
        + "\",\"compared\":" + std::to_string(compared) + ",\"compared_terrain\":" + std::to_string(compared_terrain)
        + ",\"compared_unit\":" + std::to_string(compared_unit) + ",\"compared_outside_grid\":"
        + std::to_string(compared_outside) + ",\"skipped\":" + std::to_string(skipped)
        + ",\"mismatches\":" + std::to_string(mismatches) + ",\"max_error\":" + std::to_string(max_error)
        + ",\"means\":" + means + "}");
}
