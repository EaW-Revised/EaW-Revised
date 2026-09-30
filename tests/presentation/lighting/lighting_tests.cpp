// P1-04 lighting contracts. Expected values are pinned from an independent
// float64 evaluation of the published formulas (SH basis constants,
// Ramamoorthi-Hanrahan packing, alo-viewer default environment), not from
// this implementation's output.

#include "eawr/presentation/lighting/lighting.hpp"

#include "shader_adapter.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace {

namespace lighting = eawr::presentation::lighting;
using lighting::Vec3;

int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool near(const float left, const double right, const double tolerance = 2e-5) {
    return std::abs(static_cast<double>(left) - right) <= tolerance;
}

Vec3 normalize(const Vec3 v) {
    const float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return {v.x / length, v.y / length, v.z / length};
}

// Rotation about a unit axis (Rodrigues).
Vec3 rotate(const Vec3 v, const Vec3 axis, const float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    const float dot = axis.x * v.x + axis.y * v.y + axis.z * v.z;
    const Vec3 cross{axis.y * v.z - axis.z * v.y, axis.z * v.x - axis.x * v.z, axis.x * v.y - axis.y * v.x};
    return {v.x * c + cross.x * s + axis.x * dot * (1 - c), v.y * c + cross.y * s + axis.y * dot * (1 - c),
            v.z * c + cross.z * s + axis.z * dot * (1 - c)};
}

void constant_projection() {
    const lighting::Coefficients constant = lighting::project_constant(1.0F);
    expect(near(constant[0], 3.5449077018), "a unit constant projects to L00 = 2 sqrt(pi)");
    for (std::size_t index = 1; index < 9; ++index) expect(constant[index] == 0.0F, "a constant has no higher band");
    const lighting::Matrix4 packed = lighting::pack_irradiance(constant);
    for (const Vec3 normal : {Vec3{0, 0, 1}, Vec3{1, 0, 0}, normalize({-1, 2, -3})}) {
        expect(near(lighting::evaluate(packed, normal), 3.1415929, 1e-4),
               "uniform unit radiance gives irradiance pi for every normal");
    }
}

void axis_aligned_coefficients() {
    const auto up = lighting::eval_direction({0, 0, 1});
    const std::array<double, 9> expected_up{0.282095, 0, 0.488603, 0, 0, 0, 0.630783, 0, 0};
    const auto x = lighting::eval_direction({1, 0, 0});
    const std::array<double, 9> expected_x{0.282095, 0, 0, -0.488603, 0, 0, -0.315392, 0, 0.546274};
    const auto y = lighting::eval_direction({0, 1, 0});
    const std::array<double, 9> expected_y{0.282095, -0.488603, 0, 0, 0, 0, -0.315392, 0, -0.546274};
    for (std::size_t index = 0; index < 9; ++index) {
        expect(near(up[index], expected_up[index], 1e-6), "+Z basis values (D3DX convention)");
        expect(near(x[index], expected_x[index], 1e-6), "+X basis values carry the odd-m sign");
        expect(near(y[index], expected_y[index], 1e-6), "+Y basis values carry the odd-m sign");
    }

    // A white sun straight overhead (travelling -Z) projects through the
    // negated-Z direction (0, 0, 1) and lights upward normals.
    const std::array<lighting::DirectionalLight, 1> sun{{{{0, 0, -1}, {1, 1, 1, 1}}}};
    const auto projected = lighting::project_lights(sun);
    for (std::size_t index = 0; index < 9; ++index) {
        expect(near(projected.rgb[0][index], expected_up[index], 1e-6), "overhead light coefficients");
    }
    const auto matrices = lighting::sh_matrices(sun, {0, 0, 0, 0});
    expect(near(lighting::evaluate(matrices.rgb[0], {0, 0, 1}), 1.062501, 1e-5), "overhead light, normal up");
    expect(near(lighting::evaluate(matrices.rgb[0], {0, 0, -1}), 0.0625, 1e-5), "overhead light, normal down (band-2 ringing)");
    expect(near(lighting::evaluate(matrices.rgb[0], {1, 0, 0}), 0.09375, 1e-5), "overhead light, normal sideways");
    // Irradiance peaks for the normal facing the light, for any direction.
    for (const Vec3 direction : {Vec3{1, 0, 0}, Vec3{0, -1, 0}, normalize({1, 2, -2}), normalize({-3, 1, 1})}) {
        const std::array<lighting::DirectionalLight, 1> light{{{direction, {1, 1, 1, 1}}}};
        const auto m = lighting::sh_matrices(light, {0, 0, 0, 0});
        const Vec3 toward{-direction.x, -direction.y, -direction.z};
        const float peak = lighting::evaluate(m.rgb[0], toward);
        expect(peak > lighting::evaluate(m.rgb[0], direction) + 0.9F, "facing the light is brighter than facing away");
        expect(peak > lighting::evaluate(m.rgb[0], rotate(toward, normalize({1, 1, 0}), 0.3F)), "facing the light is the peak");
    }
}

void rotation_invariance() {
    const lighting::Environment environment = lighting::alo_viewer_default_environment();
    const std::vector<Vec3> axes{normalize({0, 0, 1}), normalize({1, 0, 0}), normalize({1, -2, 3}), normalize({-2, 1, 0.5F})};
    const std::vector<Vec3> normals{{0, 0, 1}, {1, 0, 0}, normalize({1, 1, 1}), normalize({-1, 3, -2})};
    const auto base = lighting::sph_light_all(environment);
    for (const Vec3& axis : axes) {
        for (const float angle : {0.4F, 1.7F, -2.9F}) {
            lighting::Environment turned = environment;
            for (auto& light : turned.lights) light.direction = rotate(light.direction, axis, angle);
            const auto rotated = lighting::sph_light_all(turned);
            for (const Vec3& normal : normals) {
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    expect(near(lighting::evaluate(rotated.rgb[channel], rotate(normal, axis, angle)),
                                lighting::evaluate(base.rgb[channel], normal), 5e-5),
                           "irradiance is invariant when lights and normal rotate together");
                }
            }
        }
    }
}

void pinned_policies() {
    const auto sh = lighting::sph_light_all(lighting::alo_viewer_default_environment());
    const auto hemisphere = lighting::hemisphere_matrices();
    const float third = 1.0F / std::sqrt(3.0F);
    struct Row final { Vec3 normal; std::array<double, 3> sh; std::array<double, 3> hemisphere; };
    const std::array<Row, 7> rows{{
        {{0, 0, 1}, {0.446105, 0.446105, 0.451370}, {1.830526, 1.725494, 1.605452}},
        {{0, 0, -1}, {0.135963, 0.135963, 0.184640}, {0.329474, 0.314506, 0.314548}},
        {{1, 0, 0}, {0.249668, 0.249668, 0.352462}, {1.430245, 1.349231, 1.261211}},
        {{0, 1, 0}, {0.251630, 0.251630, 0.415974}, {0.519608, 0.493231, 0.478063}},
        {{-1, 0, 0}, {0.204610, 0.204610, 0.262346}, {0.729755, 0.690769, 0.658789}},
        {{0, -1, 0}, {0.437024, 0.437024, 0.433209}, {1.640392, 1.546769, 1.441937}},
        {{third, third, third}, {0.246823, 0.246823, 0.346772}, {1.391988, 1.313268, 1.228309}},
    }};
    for (const Row& row : rows) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            expect(near(lighting::evaluate(sh.rgb[channel], row.normal), row.sh[channel]),
                   "SH policy, alo-viewer default environment, pinned normal");
            // The hemisphere policy is defined in the render basis.
            expect(near(lighting::evaluate(hemisphere.rgb[channel], lighting::source_to_render(row.normal)),
                        row.hemisphere[channel]),
                   "hemisphere policy, pinned normal");
        }
    }
    // SH in the render basis evaluates the same as SH in the source basis.
    const auto render = lighting::source_to_render(sh);
    for (const Row& row : rows) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            expect(near(lighting::evaluate(render.rgb[channel], lighting::source_to_render(row.normal)), row.sh[channel]),
                   "basis conversion preserves irradiance");
        }
    }
}

void hemisphere_is_the_frozen_p0_policy() {
    namespace godot = eawr::presentation::godot_backend;
    const auto policy = lighting::hemisphere_matrices();
    const auto& raw = lighting::hemisphere_light_direction;
    const float length = std::sqrt(raw[0] * raw[0] + raw[1] * raw[1] + raw[2] * raw[2]);
    const std::array<float, 3> light{raw[0] / length, raw[1] / length, raw[2] / length};
    const std::array<godot::SphChannelMatrix, 3> frozen{godot::meshgloss_hemisphere_matrix(0.08F, 2.0F, light),
                                                        godot::meshgloss_hemisphere_matrix(0.08F, 1.88F, light),
                                                        godot::meshgloss_hemisphere_matrix(0.10F, 1.72F, light)};
    for (std::size_t channel = 0; channel < 3; ++channel) {
        for (std::size_t column = 0; column < 4; ++column) {
            for (std::size_t row = 0; row < 4; ++row) {
                expect(std::bit_cast<std::uint32_t>(policy.rgb[channel][column * 4 + row])
                           == std::bit_cast<std::uint32_t>(frozen[channel].columns[column][row]),
                       "hemisphere policy is bit-identical to the P0 adapter constants");
            }
        }
    }
}

std::vector<std::byte> bytes(const std::vector<float>& values) {
    std::vector<std::byte> out;
    for (const float value : values) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        for (unsigned shift = 0; shift < 32; shift += 8) out.push_back(static_cast<std::byte>((bits >> shift) & 255U));
    }
    return out;
}

void candidate_environment() {
    // Wholly invented field values in the candidate layout.
    std::vector<std::vector<std::byte>> storage{
        bytes({0.9F, 0.8F, 0.7F}), bytes({0.2F, 0.3F, 0.4F}), bytes({0.1F, 0.1F, 0.2F}),
        bytes({1.0F, 1.0F, 1.0F}), bytes({0.05F, 0.06F, 0.07F}),
        bytes({0.6F}), bytes({0.3F}), bytes({0.2F}),
        bytes({1.0F}), bytes({2.0F}), bytes({3.0F}),
        bytes({0.5F}), bytes({-0.2F}), bytes({-0.4F}),
    };
    std::vector<lighting::RawField> fields;
    for (std::uint32_t id = 0; id < 14; ++id) fields.push_back({id, storage[id]});
    auto environment = lighting::candidate_environment(fields);
    expect(environment.has_value(), "a complete candidate record decodes");
    if (environment) {
        expect(environment->source == lighting::EnvironmentSource::ted_candidate, "the source is marked candidate");
        expect(near(environment->lights[0].color.a, 0.6) && near(environment->lights[2].color.b, 0.2),
               "colour and intensity come from their own minis");
        // R-LIT-01: toward the light is (sin a cos e, -cos a cos e, sin e);
        // the stored direction is its negation.
        const lighting::Vec3 sun = environment->lights[0].direction;
        expect(near(sun.x, -std::sin(1.0) * std::cos(0.5)) && near(sun.y, std::cos(1.0) * std::cos(0.5))
                   && near(sun.z, -std::sin(0.5)),
               "sun direction uses heading 0x08 and elevation 0x0b in the retail convention");
        expect(near(environment->ambient.g, 0.06) && near(environment->specular.r, 1.0), "ambient and specular");
        expect(near(environment->shadow.r, 0.5) && near(environment->shadow.g, 0.5) && near(environment->shadow.b, 0.5),
               "without mini 0x17 the shadow colour is its per-record default");
    }
    std::vector<lighting::RawField> missing(fields.begin(), fields.end() - 1);
    expect(!lighting::candidate_environment(missing), "a record without every candidate field is rejected");
    std::vector<std::byte> nan = bytes({std::numeric_limits<float>::quiet_NaN()});
    std::vector<lighting::RawField> poisoned = fields;
    poisoned[8].bytes = nan;
    expect(!lighting::candidate_environment(poisoned), "a non-finite field is rejected, not repaired");
}

// --- candidate field diagnostics ---------------------------------------------

using Status = lighting::CandidateFieldStatus;

// The same invented record as candidate_environment(), owned by the fixture.
struct Record final {
    std::vector<std::vector<std::byte>> storage{
        bytes({0.9F, 0.8F, 0.7F}), bytes({0.2F, 0.3F, 0.4F}), bytes({0.1F, 0.1F, 0.2F}),
        bytes({1.0F, 1.0F, 1.0F}), bytes({0.05F, 0.06F, 0.07F}),
        bytes({0.6F}), bytes({0.3F}), bytes({0.2F}),
        bytes({1.0F}), bytes({2.0F}), bytes({3.0F}),
        bytes({0.5F}), bytes({-0.2F}), bytes({-0.4F}),
    };
    std::vector<lighting::RawField> fields() const {
        std::vector<lighting::RawField> out;
        for (std::uint32_t id = 0; id < 14; ++id) out.push_back({id, storage[id]});
        return out;
    }
};

std::size_t expected_size(const std::uint32_t id) { return id < 5 ? 12 : 4; }

bool same_bits(const float left, const float right) {
    return std::bit_cast<std::uint32_t>(left) == std::bit_cast<std::uint32_t>(right);
}

bool same_bits(const lighting::Color& left, const lighting::Color& right) {
    return same_bits(left.r, right.r) && same_bits(left.g, right.g) && same_bits(left.b, right.b)
        && same_bits(left.a, right.a);
}

bool same_bits(const lighting::Environment& left, const lighting::Environment& right) {
    bool same = left.source == right.source && same_bits(left.ambient, right.ambient)
        && same_bits(left.specular, right.specular) && same_bits(left.shadow, right.shadow);
    for (std::size_t light = 0; light < 3; ++light) {
        same = same && same_bits(left.lights[light].color, right.lights[light].color)
            && same_bits(left.lights[light].direction.x, right.lights[light].direction.x)
            && same_bits(left.lights[light].direction.y, right.lights[light].direction.y)
            && same_bits(left.lights[light].direction.z, right.lights[light].direction.z);
    }
    return same;
}

// Every result carries its own ID and size contract, in ID order.
void expect_well_formed(const lighting::CandidateEnvironmentDiagnostics& diagnostics, const std::string_view what) {
    for (std::uint32_t id = 0; id < 14; ++id) {
        const auto& field = diagnostics.fields[id];
        expect(field.id == id, what);
        expect(field.expected_size == expected_size(id), what);
        expect(field.expected_size == lighting::candidate_field_expected_size(id), what);
        const bool missing = field.status == Status::missing;
        expect(missing == (field.occurrence_count == 0) && missing == !field.selected_ordinal.has_value(), what);
        if (missing) expect(field.observed_size == 0, what);
    }
    const auto& shadow = diagnostics.shadow_color;
    expect(shadow.id == lighting::shadow_color_field_id && shadow.expected_size == 12, what);
    const bool shadow_missing = shadow.status == Status::missing;
    expect(shadow_missing == (shadow.occurrence_count == 0) && shadow_missing == !shadow.selected_ordinal.has_value(),
           what);
    bool all_decoded = shadow_missing || shadow.status == Status::decoded;
    for (const auto& field : diagnostics.fields) all_decoded = all_decoded && field.status == Status::decoded;
    expect(all_decoded == diagnostics.environment.has_value(), what);
}

void candidate_diagnostics_complete_record() {
    const Record record;
    const auto fields = record.fields();
    const auto diagnostics = lighting::diagnose_candidate_environment(fields);
    expect_well_formed(diagnostics, "complete record: well-formed results");
    for (std::uint32_t id = 0; id < 14; ++id) {
        const auto& field = diagnostics.fields[id];
        expect(field.status == Status::decoded && field.observed_size == expected_size(id)
                   && field.selected_ordinal == std::size_t{id} && field.occurrence_count == 1,
               "complete record: every field decoded once at its own ordinal");
    }
    expect(diagnostics.environment.has_value(), "complete record: environment present");
    if (!diagnostics.environment) return;
    const lighting::Environment& environment = *diagnostics.environment;

    // Independently assembled from the candidate mapping and the literals.
    lighting::Environment expected = lighting::alo_viewer_default_environment();
    expected.source = lighting::EnvironmentSource::ted_candidate;
    expected.lights[0] = {lighting::retail_light_direction(1.0F, 0.5F), {0.9F, 0.8F, 0.7F, 0.6F}};
    expected.lights[1] = {lighting::retail_light_direction(2.0F, -0.2F), {0.2F, 0.3F, 0.4F, 0.3F}};
    expected.lights[2] = {lighting::retail_light_direction(3.0F, -0.4F), {0.1F, 0.1F, 0.2F, 0.2F}};
    expected.specular = {1.0F, 1.0F, 1.0F, 1.0F};
    expected.ambient = {0.05F, 0.06F, 0.07F, 1.0F};
    expect(same_bits(environment, expected), "complete record: every value is bit-identical to the mapping");
    expect(same_bits(environment.shadow, {0.5F, 0.5F, 0.5F, 1.0F}), "complete record: default shadow 0.5 stays");
    expect(lighting::to_string(environment.source) == "ted_candidate_unconfirmed",
           "complete record: the source is labelled unconfirmed");

    const auto legacy = lighting::candidate_environment(fields);
    expect(legacy.has_value() && same_bits(*legacy, environment), "legacy reader returns the diagnostic environment");

    // SH of the decoded record against an independent float64 evaluation of
    // the same formulas (alo-viewer projection, R-H packing, ambient added).
    const auto all = lighting::sph_light_all(environment);
    const auto fill = lighting::sph_light_fill(environment);
    const float third = 1.0F / std::sqrt(3.0F);
    struct Row final { Vec3 normal; std::array<double, 3> all; std::array<double, 3> fill; };
    const std::array<Row, 5> rows{{
        {{0, 0, 1}, {0.288428, 0.272343, 0.255661}, {0.050177, 0.060565, 0.070355}},
        {{0, 0, -1}, {0.049246, 0.067887, 0.093719}, {0.069886, 0.086234, 0.109772}},
        {{1, 0, 0}, {0.496075, 0.490307, 0.487872}, {0.108030, 0.145378, 0.186060}},
        {{0, -1, 0}, {0.284177, 0.267038, 0.250450}, {0.048619, 0.057653, 0.067239}},
        {{third, third, third}, {0.306237, 0.310562, 0.321859}, {0.093007, 0.121024, 0.156013}},
    }};
    for (const Row& row : rows) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            expect(near(lighting::evaluate(all.rgb[channel], row.normal), row.all[channel]),
                   "candidate record SPH_LIGHT_ALL, pinned normal");
            expect(near(lighting::evaluate(fill.rgb[channel], row.normal), row.fill[channel]),
                   "candidate record SPH_LIGHT_FILL, pinned normal");
        }
    }
    const auto legacy_all = lighting::sph_light_all(*legacy);
    for (std::size_t channel = 0; channel < 3; ++channel) {
        for (std::size_t index = 0; index < 16; ++index) {
            expect(same_bits(legacy_all.rgb[channel][index], all.rgb[channel][index]),
                   "legacy and diagnostic environments give bit-identical SH matrices");
        }
    }
}

void candidate_diagnostics_missing() {
    const auto empty = lighting::diagnose_candidate_environment({});
    expect_well_formed(empty, "empty list: well-formed results");
    expect(!empty.environment && !lighting::candidate_environment({}), "empty list: no environment");
    for (const auto& field : empty.fields) {
        expect(field.status == Status::missing && field.occurrence_count == 0, "empty list: every field missing");
    }

    // Only unknown IDs, including ones whose low byte aliases a candidate ID.
    const std::vector<std::byte> twelve = bytes({1.0F, 1.0F, 1.0F});
    const std::vector<lighting::RawField> unknown{{0x0eU, twelve}, {0x100U, twelve}, {0xffffffffU, twelve},
                                                  {0x10dU, twelve}, {0x0fU, twelve}};
    const auto only_unknown = lighting::diagnose_candidate_environment(unknown);
    expect_well_formed(only_unknown, "unknown IDs only: well-formed results");
    for (const auto& field : only_unknown.fields) {
        expect(field.status == Status::missing, "unknown IDs are not read as candidate fields");
    }
    expect(lighting::candidate_field_expected_size(0x0eU) == 0, "IDs past 0x0d have no candidate size");

    // Each field removed on its own.
    const Record record;
    for (std::uint32_t id = 0; id < 14; ++id) {
        auto fields = record.fields();
        fields.erase(fields.begin() + id);
        const auto diagnostics = lighting::diagnose_candidate_environment(fields);
        expect_well_formed(diagnostics, "one missing: well-formed results");
        expect(!diagnostics.environment && !lighting::candidate_environment(fields), "one missing: no environment");
        for (std::uint32_t other = 0; other < 14; ++other) {
            const auto& field = diagnostics.fields[other];
            if (other == id) {
                expect(field.status == Status::missing, "one missing: the removed field is missing");
            } else {
                expect(field.status == Status::decoded
                           && field.selected_ordinal == std::size_t{other < id ? other : other - 1},
                       "one missing: the others still decode at their shifted ordinals");
            }
        }
    }
}

void candidate_diagnostics_sizes() {
    const Record record;
    for (std::uint32_t id = 0; id < 14; ++id) {
        const std::size_t size = expected_size(id);
        for (const std::size_t observed : {std::size_t{0}, size - 1, size - 4, size + 1, size + 4}) {
            std::vector<std::byte> wrong(observed, std::byte{0});
            auto fields = record.fields();
            fields[id].bytes = wrong;
            const auto diagnostics = lighting::diagnose_candidate_environment(fields);
            expect_well_formed(diagnostics, "wrong size: well-formed results");
            const auto& field = diagnostics.fields[id];
            expect(field.status == Status::wrong_size && field.observed_size == observed
                       && field.expected_size == size && field.selected_ordinal == std::size_t{id},
                   "short or oversized field is wrong_size with its observed size");
            expect(!diagnostics.environment && !lighting::candidate_environment(fields),
                   "a wrong-size field rejects the environment");
        }
    }
    // A float3 where a float is expected, and the reverse.
    auto fields = record.fields();
    const std::vector<std::byte> float3 = bytes({0.1F, 0.2F, 0.3F});
    const std::vector<std::byte> float1 = bytes({0.1F});
    fields[0x05].bytes = float3;
    fields[0x03].bytes = float1;
    const auto swapped = lighting::diagnose_candidate_environment(fields);
    expect(swapped.fields[0x05].status == Status::wrong_size && swapped.fields[0x05].observed_size == 12
               && swapped.fields[0x03].status == Status::wrong_size && swapped.fields[0x03].observed_size == 4,
           "float3/float confusion is wrong_size both ways");
}

void candidate_diagnostics_nonfinite() {
    const Record record;
    constexpr float inf = std::numeric_limits<float>::infinity();
    const std::array<float, 4> poisons{std::numeric_limits<float>::quiet_NaN(),
                                       std::numeric_limits<float>::signaling_NaN(), inf, -inf};
    for (std::uint32_t id = 0; id < 14; ++id) {
        const std::size_t components = expected_size(id) / 4;
        for (std::size_t component = 0; component < components; ++component) {
            for (const float poison : poisons) {
                std::vector<float> values(components, 0.25F);
                values[component] = poison;
                const std::vector<std::byte> poisoned = bytes(values);
                auto fields = record.fields();
                fields[id].bytes = poisoned;
                const auto diagnostics = lighting::diagnose_candidate_environment(fields);
                expect_well_formed(diagnostics, "non-finite: well-formed results");
                const auto& field = diagnostics.fields[id];
                expect(field.status == Status::nonfinite && field.observed_size == expected_size(id),
                       "NaN/+Inf/-Inf in any component is nonfinite, not wrong_size");
                expect(!diagnostics.environment && !lighting::candidate_environment(fields),
                       "a non-finite component rejects the environment");
            }
        }
    }
    // Finite extremes are shape-valid: no range validation is invented.
    auto fields = record.fields();
    const std::vector<std::byte> extreme = bytes({std::numeric_limits<float>::max(), -1.0e30F,
                                                  std::numeric_limits<float>::denorm_min()});
    const std::vector<std::byte> negative = bytes({-7.0F});
    fields[0x01].bytes = extreme;
    fields[0x06].bytes = negative;
    const auto diagnostics = lighting::diagnose_candidate_environment(fields);
    expect(diagnostics.environment.has_value() && diagnostics.fields[0x01].status == Status::decoded
               && diagnostics.fields[0x06].status == Status::decoded,
           "finite out-of-range values still decode");
    if (diagnostics.environment) {
        expect(same_bits(diagnostics.environment->lights[1].color.r, std::numeric_limits<float>::max())
                   && same_bits(diagnostics.environment->lights[1].color.a, -7.0F),
               "finite out-of-range values are carried unchanged, not clamped");
    }
}

void candidate_diagnostics_multiple_failures() {
    const Record record;
    auto fields = record.fields();
    const std::vector<std::byte> nan = bytes({std::numeric_limits<float>::quiet_NaN()});
    const std::vector<std::byte> inf3 = bytes({0.0F, -std::numeric_limits<float>::infinity(), 0.0F});
    const std::vector<std::byte> short3(8, std::byte{0});
    fields[0x0b].bytes = nan;
    fields[0x02].bytes = inf3;
    fields[0x07].bytes = short3;
    fields.erase(fields.begin() + 0x0d);
    fields.erase(fields.begin() + 0x04);
    // Reverse the input and add unknown IDs; results stay in ID order.
    std::vector<lighting::RawField> reversed(fields.rbegin(), fields.rend());
    const std::vector<std::byte> junk(3, std::byte{0xff});
    reversed.insert(reversed.begin() + 3, {0x20U, junk});
    reversed.push_back({0x0eU, junk});

    const auto diagnostics = lighting::diagnose_candidate_environment(reversed);
    expect_well_formed(diagnostics, "multiple failures: well-formed results");
    expect(!diagnostics.environment, "multiple failures: no environment");
    const std::array<Status, 14> expected{Status::decoded, Status::decoded, Status::nonfinite, Status::decoded,
                                          Status::missing, Status::decoded, Status::decoded, Status::wrong_size,
                                          Status::decoded, Status::decoded, Status::decoded, Status::nonfinite,
                                          Status::decoded, Status::missing};
    for (std::uint32_t id = 0; id < 14; ++id) {
        expect(diagnostics.fields[id].status == expected[id], "multiple failures are all reported, in ID order");
        if (diagnostics.fields[id].selected_ordinal) {
            const std::size_t ordinal = *diagnostics.fields[id].selected_ordinal;
            expect(ordinal < reversed.size() && reversed[ordinal].id == id,
                   "the selected ordinal points at the input field of that ID");
        }
    }
    expect(diagnostics.fields[0x07].observed_size == 8, "multiple failures: short size reported");
    expect(lighting::to_string(Status::decoded) == "decoded" && lighting::to_string(Status::missing) == "missing"
               && lighting::to_string(Status::wrong_size) == "wrong_size"
               && lighting::to_string(Status::nonfinite) == "nonfinite",
           "status labels");
}

void candidate_diagnostics_duplicates() {
    const Record record;
    const std::vector<std::byte> valid_other = bytes({0.75F});
    const std::vector<std::byte> nan = bytes({std::numeric_limits<float>::quiet_NaN()});
    const std::vector<std::byte> short1(2, std::byte{0});

    struct Case final {
        std::string_view name;
        std::vector<std::span<const std::byte>> occurrences; // after the record's own 0x06 is replaced
        Status status;
        std::size_t observed;
        bool decodes;
    };
    const std::vector<std::byte>& own = record.storage[0x06];
    const std::vector<Case> cases{
        {"valid then valid", {own, valid_other}, Status::decoded, 4, true},
        {"valid then malformed", {own, nan}, Status::decoded, 4, true},
        {"valid then short", {own, short1}, Status::decoded, 4, true},
        {"malformed then valid", {nan, own}, Status::nonfinite, 4, false},
        {"short then valid", {short1, own}, Status::wrong_size, 2, false},
        {"short then malformed then valid", {short1, nan, own}, Status::wrong_size, 2, false},
        {"valid, valid, valid", {valid_other, own, own}, Status::decoded, 4, true},
    };
    for (const Case& test : cases) {
        // Place the occurrences at the front, the middle and the back.
        for (const std::size_t position : {std::size_t{0}, std::size_t{7}, std::size_t{13}}) {
            auto fields = record.fields();
            fields.erase(fields.begin() + 0x06);
            std::vector<lighting::RawField> input;
            std::size_t first_ordinal = 0;
            for (std::size_t index = 0; index <= fields.size(); ++index) {
                if (index == position) {
                    first_ordinal = input.size();
                    input.push_back({0x06U, test.occurrences[0]});
                }
                if (index < fields.size()) input.push_back(fields[index]);
            }
            // Later occurrences go after every other field.
            for (std::size_t later = 1; later < test.occurrences.size(); ++later) {
                input.push_back({0x06U, test.occurrences[later]});
            }
            const auto diagnostics = lighting::diagnose_candidate_environment(input);
            expect_well_formed(diagnostics, test.name);
            const auto& field = diagnostics.fields[0x06];
            expect(field.status == test.status && field.observed_size == test.observed, test.name);
            expect(field.selected_ordinal == first_ordinal, "the FIRST occurrence is the one selected");
            expect(field.occurrence_count == test.occurrences.size(), "every occurrence is counted");
            expect(diagnostics.environment.has_value() == test.decodes, test.name);
            expect(lighting::candidate_environment(input).has_value() == test.decodes,
                   "the legacy reader applies the same first-occurrence rule");
            if (diagnostics.environment) {
                const float first = std::bit_cast<float>(std::array<std::byte, 4>{
                    test.occurrences[0][0], test.occurrences[0][1], test.occurrences[0][2], test.occurrences[0][3]});
                expect(same_bits(diagnostics.environment->lights[1].color.a, first),
                       "the decoded value comes from the first occurrence");
            }
        }
    }
    // Duplicates of other IDs never disturb a field's own count.
    auto fields = record.fields();
    fields.push_back({0x0dU, record.storage[0x0d]});
    fields.push_back({0x00U, short1});
    const auto diagnostics = lighting::diagnose_candidate_environment(fields);
    expect(diagnostics.environment.has_value() && diagnostics.fields[0x0d].occurrence_count == 2
               && diagnostics.fields[0x00].occurrence_count == 2 && diagnostics.fields[0x06].occurrence_count == 1
               && diagnostics.fields[0x00].selected_ordinal == std::size_t{0},
           "trailing duplicates are counted without changing the first selection");
}

void candidate_diagnostics_outlive_input() {
    lighting::CandidateEnvironmentDiagnostics diagnostics;
    {
        auto record = std::make_unique<Record>();
        record->storage[0x09] = bytes({std::numeric_limits<float>::infinity()});
        record->storage[0x03].resize(13);
        const auto fields = record->fields();
        diagnostics = lighting::diagnose_candidate_environment(fields);
        // Scribble over the input before releasing it.
        for (auto& storage : record->storage) std::fill(storage.begin(), storage.end(), std::byte{0x7f});
    }
    expect_well_formed(diagnostics, "released input: well-formed results");
    expect(diagnostics.fields[0x09].status == Status::nonfinite && diagnostics.fields[0x03].status == Status::wrong_size
               && diagnostics.fields[0x03].observed_size == 13 && diagnostics.fields[0x00].status == Status::decoded,
           "results survive the input's release unchanged");

    std::optional<lighting::Environment> environment;
    {
        const Record record;
        const auto fields = record.fields();
        environment = lighting::diagnose_candidate_environment(fields).environment;
    }
    expect(environment.has_value() && same_bits(environment->ambient, {0.05F, 0.06F, 0.07F, 1.0F}),
           "the decoded environment survives the input's release");
}

// Mini 0x17 (#225): the shadow colour, per record, default 0.5 grey.
void candidate_shadow_color() {
    const Record record;
    const auto base = record.fields();
    const auto absent = lighting::diagnose_candidate_environment(base);
    expect_well_formed(absent, "0x17 absent: well-formed results");
    expect(absent.shadow_color.status == Status::missing && absent.environment
               && same_bits(absent.environment->shadow, {0.5F, 0.5F, 0.5F, 1.0F}),
           "0x17 absent: the record keeps the default (0.5, 0.5, 0.5)");

    // Naboo Sunrise_Clear's colour, appended after the light fields.
    const std::vector<std::byte> sunrise = bytes({0.4F, 0.4157F, 0.6F});
    auto fields = base;
    fields.push_back({lighting::shadow_color_field_id, sunrise});
    const auto decoded = lighting::diagnose_candidate_environment(fields);
    expect_well_formed(decoded, "0x17 present: well-formed results");
    expect(decoded.shadow_color.status == Status::decoded && decoded.shadow_color.selected_ordinal == std::size_t{14}
               && decoded.shadow_color.observed_size == 12,
           "0x17 present: decoded at its own ordinal");
    expect(decoded.environment && same_bits(decoded.environment->shadow, {0.4F, 0.4157F, 0.6F, 1.0F}),
           "0x17 present: every channel is the stored float, bit for bit");
    const auto legacy = lighting::candidate_environment(fields);
    expect(legacy && decoded.environment && same_bits(legacy->shadow, decoded.environment->shadow),
           "legacy reader decodes 0x17 too");
    expect(decoded.environment && absent.environment
               && same_bits(decoded.environment->ambient, absent.environment->ambient)
               && same_bits(decoded.environment->lights[0].color, absent.environment->lights[0].color),
           "0x17 changes only the shadow colour");

    // Present but short, oversized or non-finite rejects the record (R-DEC-04).
    for (const std::size_t observed : {std::size_t{0}, std::size_t{4}, std::size_t{11}, std::size_t{13}}) {
        const std::vector<std::byte> wrong(observed, std::byte{0});
        auto sized = base;
        sized.push_back({lighting::shadow_color_field_id, wrong});
        const auto diagnostics = lighting::diagnose_candidate_environment(sized);
        expect_well_formed(diagnostics, "0x17 wrong size: well-formed results");
        expect(diagnostics.shadow_color.status == Status::wrong_size
                   && diagnostics.shadow_color.observed_size == observed && !diagnostics.environment
                   && !lighting::candidate_environment(sized),
               "0x17 of the wrong size rejects the environment");
    }
    const std::vector<std::byte> poisoned = bytes({0.4F, std::numeric_limits<float>::quiet_NaN(), 0.6F});
    auto nan = base;
    nan.push_back({lighting::shadow_color_field_id, poisoned});
    const auto rejected = lighting::diagnose_candidate_environment(nan);
    expect(rejected.shadow_color.status == Status::nonfinite && !rejected.environment,
           "a non-finite 0x17 channel rejects the environment, not repaired");

    // The reader's first-occurrence rule holds for 0x17 as for the light fields.
    const std::vector<std::byte> noon = bytes({0.651F, 0.6431F, 0.7255F});
    auto repeated = fields;
    repeated.push_back({lighting::shadow_color_field_id, noon});
    const auto first = lighting::diagnose_candidate_environment(repeated);
    expect(first.shadow_color.occurrence_count == 2 && first.shadow_color.selected_ordinal == std::size_t{14}
               && first.environment && same_bits(first.environment->shadow, {0.4F, 0.4157F, 0.6F, 1.0F}),
           "a repeated 0x17: the first occurrence is read, the rest counted");

    // Missing light fields still reject, whatever 0x17 holds.
    auto incomplete = fields;
    incomplete.erase(incomplete.begin());
    expect(!lighting::candidate_environment(incomplete), "0x17 does not stand in for a missing light field");
}

void defaults_and_parsing() {
    const auto environment = lighting::alo_viewer_default_environment();
    // alo-viewer: sun = -Vector3(-90 deg, 45 deg).
    expect(near(environment.lights[0].direction.x, 0.0, 1e-6) && near(environment.lights[0].direction.y, 0.707107)
               && near(environment.lights[0].direction.z, -0.707107), "default sun direction");
    expect(lighting::parse_policy("sh") == lighting::Policy::sh && lighting::parse_policy("hemisphere")
               == lighting::Policy::hemisphere && lighting::parse_policy("off") == lighting::Policy::off
               && !lighting::parse_policy("SH"), "policy parsing is exact");
}

void retail_sun_specular() {
    lighting::Environment environment{};
    environment.specular = {0.25F, 0.5F, 0.75F, 0.9F};
    for (const float intensity : {0.0F, 0.25F, 0.5F, 1.5F}) {
        environment.lights[0].color = {0.1F, 0.2F, 0.3F, intensity};
        const auto value = lighting::sun_specular(environment);
        expect(near(value[0], 0.5F * intensity) && near(value[1], intensity)
            && near(value[2], 1.5F * intensity),
            "R-LIT-04: specular is scaled by twice sun intensity, including zero and HDR");
    }
}

void retail_light_heading() {
    // R-LIT-01: heading 0 puts the light toward -Y and a quarter turn puts it
    // toward +X; the stored direction is where the light travels.
    const Vec3 north = lighting::retail_light_direction(0.0F, 0.0F);
    expect(near(north.x, 0.0, 1e-6) && near(north.y, 1.0) && near(north.z, 0.0, 1e-6),
           "R-LIT-01: heading 0 lights from -Y");
    const Vec3 east = lighting::retail_light_direction(1.5707963F, 0.0F);
    expect(near(east.x, -1.0) && near(east.y, 0.0, 1e-6) && near(east.z, 0.0, 1e-6),
           "R-LIT-01: heading pi/2 lights from +X");
    const Vec3 overhead = lighting::retail_light_direction(2.0F, 1.5707963F);
    expect(near(overhead.z, -1.0) && near(overhead.x, 0.0, 1e-6) && near(overhead.y, 0.0, 1e-6),
           "R-LIT-01: elevation pi/2 lights from straight above");
    // R-LIT-02: alo-viewer's formula gives the same vector a quarter turn
    // earlier in heading, never the TED heading itself.
    for (const float heading : {0.0F, 0.7F, 2.5F, 4.0F, 6.1F}) {
        for (const float elevation : {-0.4F, 0.0F, 0.9F}) {
            const Vec3 retail = lighting::retail_light_direction(heading, elevation);
            const Vec3 alo = lighting::direction_from_angles(heading - 1.5707963F, elevation);
            expect(near(retail.x, alo.x) && near(retail.y, alo.y) && near(retail.z, alo.z),
                   "R-LIT-02: retail heading is alo-viewer's heading plus 90 degrees");
        }
    }
}

} // namespace

int main() {
    retail_sun_specular();
    retail_light_heading();
    constant_projection();
    axis_aligned_coefficients();
    rotation_invariance();
    pinned_policies();
    hemisphere_is_the_frozen_p0_policy();
    candidate_environment();
    candidate_diagnostics_complete_record();
    candidate_diagnostics_missing();
    candidate_diagnostics_sizes();
    candidate_diagnostics_nonfinite();
    candidate_diagnostics_multiple_failures();
    candidate_diagnostics_duplicates();
    candidate_diagnostics_outlive_input();
    candidate_shadow_color();
    defaults_and_parsing();
    if (failures != 0) {
        std::cerr << failures << " lighting contract(s) failed\n";
        return 1;
    }
    std::cout << "lighting contracts passed\n";
    return 0;
}
