// Distance fog contracts (R-DEC-08, R-FOG-01..05 and case C-08 of docs/behaviour/p1-effective-environment.md).

#include "eawr/presentation/lighting/distance_fog.hpp"

#include "distance_fog_source.hpp"
#include "shader_adapter.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace eawr::presentation::lighting;

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

[[nodiscard]] bool near_value(const float a, const float b, const float tolerance = 1e-5F) {
    return std::fabs(a - b) <= tolerance;
}

std::vector<std::byte> floats(const std::vector<float>& values) {
    std::vector<std::byte> bytes(values.size() * 4U);
    for (std::size_t i = 0; i < values.size(); ++i) std::memcpy(bytes.data() + i * 4U, &values[i], 4U);
    return bytes;
}

struct Record final {
    std::vector<std::vector<std::byte>> storage;
    std::vector<RawField> fields;
    void add(const std::uint32_t id, std::vector<std::byte> bytes) {
        storage.push_back(std::move(bytes));
        fields.push_back({id, std::span<const std::byte>(storage.back())});
        // Re-point every span: the vector may have moved.
        for (std::size_t i = 0; i < fields.size(); ++i) fields[i].bytes = std::span<const std::byte>(storage[i]);
    }
};

void value_contracts() {
    DistanceFog fog;
    fog.enabled = true;
    fog.start_distance = 400.0F;
    fog.end_distance = 2400.0F;
    const FogVals vals = fog_vals(fog);
    expect(near_value(vals.x, -0.0005F) && near_value(vals.y, 1.2F), "case C-08: FOG_VALS = (-0.0005, 1.2)");
    expect(near_value(fog_factor(vals, 400.0F), 1.0F), "factor 1 at the start distance");
    expect(near_value(fog_factor(vals, 1400.0F), 0.5F), "factor 0.5 midway");
    expect(near_value(fog_factor(vals, 2400.0F), 0.0F), "factor 0 at the end distance");
    expect(fog_factor(vals, 100.0F) == 1.0F && fog_factor(vals, 9000.0F) == 0.0F, "the factor saturates on both sides");

    fog.enabled = false;
    expect(fog_vals(fog) == FogVals{} && fog_factor(fog_vals(fog), 1.0e6F) == 1.0F, "disabled fog is the neutral constants");
    fog.enabled = true;
    fog.start_distance = fog.end_distance;
    expect(fog_vals(fog) == FogVals{}, "an empty range (unverified in retail) draws no fog");

    fog = DistanceFog{true, {0.8F, 0.9F, 1.0F}, 100.0F, 300.0F};
    const auto none = fogged_color({0.1F, 0.2F, 0.3F}, fog, 50.0F);
    expect(near_value(none[0], 0.1F) && near_value(none[1], 0.2F) && near_value(none[2], 0.3F), "inside the start the colour is unchanged");
    const auto half = fogged_color({0.0F, 0.0F, 0.0F}, fog, 200.0F);
    expect(near_value(half[0], 0.4F) && near_value(half[2], 0.5F), "midway the colour is half way to the fog colour");
    const auto full = fogged_color({0.0F, 0.0F, 0.0F}, fog, 5000.0F);
    expect(near_value(full[0], 0.8F) && near_value(full[1], 0.9F) && near_value(full[2], 1.0F), "beyond the end it is the fog colour");
}

void decode_contracts() {
    Record record;
    record.add(fog_color_field_id, floats({0.5F, 0.6F, 0.7F}));
    record.add(fog_enabled_field_id, {std::byte{1}});
    record.add(fog_start_field_id, floats({400.0F}));
    record.add(fog_end_field_id, floats({2400.0F}));
    auto fog = distance_fog(record.fields);
    expect(fog && fog->enabled && near_value(fog->start_distance, 400.0F) && near_value(fog->end_distance, 2400.0F)
               && near_value(fog->color[0], 0.5F) && near_value(fog->color[2], 0.7F),
           "a full record decodes");

    Record defaults;
    fog = distance_fog(defaults.fields);
    expect(fog && !fog->enabled && near_value(fog->start_distance, 1000.0F) && near_value(fog->end_distance, 10000.0F)
               && near_value(fog->color[0], 1.0F),
           "an empty record has the defaults: off, 1000 to 10000, white (R-DEC-07)");

    Record capped;
    capped.add(fog_enabled_field_id, {std::byte{1}});
    capped.add(fog_start_field_id, floats({25000.0F}));
    capped.add(fog_end_field_id, floats({12000.0F}));
    fog = distance_fog(capped.fields);
    expect(fog && near_value(fog->start_distance, 10000.0F) && near_value(fog->end_distance, 12000.0F),
           "start is capped at 10000 and the end is never below it (R-DEC-08)");

    Record repeated;
    repeated.add(fog_start_field_id, floats({100.0F}));
    repeated.add(fog_start_field_id, floats({200.0F}));
    repeated.add(fog_start_field_id, std::vector<std::byte>(2, std::byte{0})); // short: ignored, keeps 200
    fog = distance_fog(repeated.fields);
    expect(fog && near_value(fog->start_distance, 200.0F), "the last valid occurrence wins; a short scalar keeps the previous value (R-DEC-02, R-DEC-04)");

    Record bad_color;
    bad_color.add(fog_color_field_id, floats({0.5F, 0.5F}));
    expect(!distance_fog(bad_color.fields), "a short colour makes the record unreadable");
    Record nan_color;
    nan_color.add(fog_color_field_id, floats({0.5F, std::nanf(""), 0.5F}));
    expect(!distance_fog(nan_color.fields), "a non-finite colour makes the record unreadable");
}

void program_contracts() {
    expect(program_fades_to_fog("MeshGloss.fx") && program_fades_to_fog("meshgloss.fxo") && program_fades_to_fog("data/art/shaders/BatchMeshGloss.fx"),
           "the opaque gloss programs are fogged, whatever the spelling");
    expect(program_fades_to_fog("RSkinGloss.fx") && program_fades_to_fog("Planet.fx"), "skinned gloss and the planet are fogged");
    expect(!program_fades_to_fog("MeshAdditive.fx") && !program_fades_to_fog("MeshShield.fx") && !program_fades_to_fog("Skydome.fx"),
           "additive, shield and sky programs write a constant fog output and stay unfogged");
    expect(!program_fades_to_fog("") && !program_fades_to_fog("Unknown.fx"), "unknown names are unfogged");
}

void source_contracts() {
    namespace source = eawr::presentation::godot_backend::distance_fog_source;
    using eawr::presentation::godot_backend::meshgloss_shader_alpha;
    using eawr::presentation::godot_backend::meshgloss_shader_opaque;
    const auto opaque = source::fogged(meshgloss_shader_opaque);
    expect(opaque.has_value(), "the opaque MeshGloss adapter can be fogged");
    if (opaque) {
        const std::string& text = *opaque;
        expect(text.find("global uniform vec2 eawr_distance_fog_vals;") != std::string::npos
                   && text.find("global uniform vec3 eawr_distance_fog_color;") != std::string::npos
                   && text.find("varying vec3 eawr_distance_fog_view;") != std::string::npos,
               "the shared constants and the view-space position are declared");
        const auto vertex_end = text.find("void fragment()");
        const auto view = text.find("eawr_distance_fog_view = vec3(abs(PROJECTION_MATRIX[0][0])");
        expect(view != std::string::npos && view < vertex_end, "the view-space position is written in the vertex stage");
        const auto factor = text.find("float eawr_distance_fog_factor = clamp(");
        expect(factor != std::string::npos && factor > vertex_end, "the factor is computed per pixel in the fragment stage");
        const auto blend = text.rfind("ALBEDO = mix(eawr_distance_fog_color, ALBEDO, eawr_distance_fog_factor);");
        const auto last_brace = text.rfind('}');
        expect(blend != std::string::npos && blend < last_brace && text.find("ALBEDO = eawr_linear_to_srgb") < blend,
               "the blend is the last statement of the fragment stage, after the adapter's own ALBEDO write");
        expect(!source::fogged(text), "an already fogged source is not fogged twice");
        expect(text.size() > std::string_view(meshgloss_shader_opaque).size(), "the source only grows");
    }
    expect(source::fogged(meshgloss_shader_alpha).has_value(), "the blended (alpha) MeshGloss adapter can be fogged");
    expect(!source::fogged(R"(shader_type spatial;
render_mode blend_add;
void fragment() { ALBEDO = vec3(1.0); }
)"),
           "an additive source is left alone");
    expect(!source::fogged(R"(shader_type spatial;
void vertex() { }
)"), "a source without a fragment stage is left alone");
    const auto no_vertex = source::fogged(R"(shader_type spatial;
void fragment() { ALBEDO = vec3(0.5); }
)");
    expect(no_vertex && no_vertex->find("void vertex()") != std::string::npos
               && no_vertex->find("void vertex()") < no_vertex->find("void fragment()"),
           "a source without a vertex stage gets one that only computes the factor");
}

} // namespace

void fog_globals_never_carry_over() {
    DistanceFog fogged;
    fogged.enabled = true;
    fogged.color = {0.2F, 0.4F, 0.8F};
    fogged.start_distance = 500.0F;
    fogged.end_distance = 5000.0F;
    const FogGlobals on = fog_globals(fogged);
    expect(!(on == FogGlobals{}) && on.color == fogged.color, "a fogged scene sets its constants and colour");
    // The next scene: no record, a disabled record and an unusable range all go back to neutral.
    expect(fog_globals(std::nullopt) == FogGlobals{}, "a scene without fog sets neutral values (0, 1) and white");
    DistanceFog disabled = fogged;
    disabled.enabled = false;
    expect(fog_globals(disabled) == FogGlobals{}, "a disabled fog sets neutral values");
    DistanceFog empty_range = fogged;
    empty_range.end_distance = empty_range.start_distance;
    expect(fog_globals(empty_range) == FogGlobals{}, "an unusable range sets neutral values");
    expect(fog_factor(fog_globals(std::nullopt).vals, 123456.0F) == 1.0F, "neutral values give factor 1 at any distance");
}

int main() {
    value_contracts();
    decode_contracts();
    program_contracts();
    source_contracts();
    fog_globals_never_carry_over();
    if (failures != 0) { std::cerr << failures << " distance fog check(s) failed\n"; return 1; }
    std::cout << "distance fog contracts passed\n";
    return 0;
}
