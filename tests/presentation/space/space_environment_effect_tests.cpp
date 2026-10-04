#include "space_effect_support.hpp"

namespace eawr_space_test {

void test_environment_effect_routes() {
    using eawr::assets::MaterialParameter;
    using eawr::assets::ParameterKind;
    using eawr::assets::Vec4f;
    const auto binding_names = [](const space::EnvironmentEffectPlan& plan) {
        std::vector<std::string> names;
        for (const auto& binding : plan.material.bindings) names.push_back(binding.name);
        return names;
    };
    space::EnvironmentEffectInputs inputs;
    inputs.id = "synthetic-explicit-inputs";
    inputs.time_seconds = 0.25F;
    inputs.light_scale = {1, 1, 1, 1};
    inputs.light_direction = {1, 0, 0};
    inputs.ambient_light = {0.2F, 0.2F, 0.2F};
    inputs.diffuse_light = {0.8F, 0.8F, 0.8F};
    inputs.specular_light = {1, 1, 1};

    auto model = sky_model();
    auto& surface = model.meshes[0].submeshes[0];
    for (auto& vertex : surface.vertices) {
        vertex.normal = {1, 0, 0};
        vertex.color = {0.5F, 0.25F, 0.75F, 1};
    }
    surface.shader = "Planet.fx";
    surface.vertex_format = "alD3dVertNU2U3U3";
    surface.parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("cloud.tga")});
    surface.parameters.push_back({"Emissive", ParameterKind::vector3, Vec3f{0.1F, 0.2F, 0.3F}});
    surface.parameters.push_back({"Diffuse", ParameterKind::vector3, Vec3f{0.6F, 0.7F, 0.8F}});
    surface.parameters.push_back({"Specular", ParameterKind::vector3, Vec3f{0.3F, 0.2F, 0.1F}});
    surface.parameters.push_back({"CloudScrollRate", ParameterKind::scalar, 0.4F});
    const auto planet = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(planet.status == space::EnvironmentEffectStatus::ready && planet.route_id == space::planet_route_id
               && planet.pass_name == "t0_p0" && planet.textures.size() == 2
               && planet.material.pass == eawr::presentation::RenderPass::opaque
               && planet.fields.size() == 6,
           "Planet t0 binds two textures and three authored colors under explicit lighting");
    expect(std::any_of(planet.fields.begin(), planet.fields.end(), [](const auto& field) {
        return field.name == "CloudScrollRate" && field.disposition == "recorded_not_consumed";
    }), "Planet t0 records its unused cloud scroll field");
    auto mixed_planet = model;
    auto& mixed_planet_parameters = mixed_planet.meshes[0].submeshes[0].parameters;
    mixed_planet_parameters[0].name = "basetexture";
    mixed_planet_parameters[1].name = "cLoUdTeXtUrE";
    mixed_planet_parameters[2].name = "emissive";
    mixed_planet_parameters[3].name = "DIFFUSE";
    mixed_planet_parameters[4].name = "sPeCuLaR";
    mixed_planet_parameters[5].name = "cloudscrollrate";
    const auto canonical_planet = space::plan_environment_effect(mixed_planet, 0, 0, "t0", inputs);
    expect(canonical_planet.status == space::EnvironmentEffectStatus::ready
               && canonical_planet.textures == std::vector<std::pair<std::string, std::string>>{
                   {"BaseTexture", "eawr_space_a.tga"}, {"CloudTexture", "cloud.tga"}}
               && binding_names(canonical_planet) == std::vector<std::string>{
                   "BaseTexture", "CloudTexture", "Emissive", "Diffuse", "Specular",
                   "eawr_effect_time", "eawr_effect_light_scale", "eawr_effect_light_direction",
                   "eawr_effect_ambient", "eawr_effect_diffuse", "eawr_effect_specular"}
               && canonical_planet.fields[5].name == "CloudScrollRate",
           "Planet mixed-case fields bind the canonical shader uniform names");
    expect(space::plan_environment_effect(model, 0, 0, "sph_t2", inputs).status
               == space::EnvironmentEffectStatus::technique_unsupported,
           "Planet programmable technique stays closed");
    inputs.light_direction = {0, 0, 0};
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::input_invalid,
           "Planet rejects a zero light direction");
    inputs.light_direction = {1, 0, 0};
    inputs.light_scale.w = 0.5F;
    const auto blended = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(blended.status == space::EnvironmentEffectStatus::ready
               && blended.material.pass == eawr::presentation::RenderPass::transparent
               && blended.material.program.find("ALPHA =") != std::string::npos,
           "Planet alpha branch selects explicit SRCALPHA blend path");
    inputs.light_scale.w = 1;
    surface.parameters.push_back({"CloudTexture", ParameterKind::texture, std::string("duplicate.tga")});
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_invalid, "duplicate Planet texture blocks");
    surface.parameters.pop_back();
    surface.parameters.erase(surface.parameters.begin() + 1);
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_missing, "missing Planet cloud texture blocks");

    surface.shader = "Nebula.fx";
    surface.vertex_format = "alD3dVertNU2C";
    surface.parameters.clear();
    surface.parameters.push_back({"BaseTexture", ParameterKind::texture, std::string("nebula.tga")});
    surface.parameters.push_back({"UVScrollRate", ParameterKind::vector4, Vec4f{0.25F, -0.5F, 0, 0}});
    surface.parameters.push_back({"DistortionScale", ParameterKind::scalar, 2.0F});
    surface.parameters.push_back({"SFreq", ParameterKind::scalar, 0.1F});
    surface.parameters.push_back({"TFreq", ParameterKind::scalar, 0.2F});
    const auto nebula = space::plan_environment_effect(model, 0, 0, "t0", inputs);
    expect(nebula.status == space::EnvironmentEffectStatus::ready && nebula.route_id == space::nebula_route_id
               && nebula.textures.size() == 1
               && nebula.material.pass == eawr::presentation::RenderPass::transparent
               && nebula.material.program.find("blend_add") != std::string::npos,
           "Nebula t0 binds authored scroll, distortion, and additive state");
    auto mixed_nebula = model;
    auto& mixed_nebula_parameters = mixed_nebula.meshes[0].submeshes[0].parameters;
    mixed_nebula_parameters[0].name = "basetexture";
    mixed_nebula_parameters[1].name = "uvscrollrate";
    mixed_nebula_parameters[2].name = "DISTORTIONSCALE";
    mixed_nebula_parameters[3].name = "sfreq";
    mixed_nebula_parameters[4].name = "tFrEq";
    const auto canonical_nebula = space::plan_environment_effect(mixed_nebula, 0, 0, "t0", inputs);
    expect(canonical_nebula.status == space::EnvironmentEffectStatus::ready
               && canonical_nebula.textures == std::vector<std::pair<std::string, std::string>>{
                   {"BaseTexture", "nebula.tga"}}
               && binding_names(canonical_nebula) == std::vector<std::string>{
                   "BaseTexture", "UVScrollRate", "DistortionScale", "SFreq", "TFreq",
                   "eawr_effect_time", "eawr_effect_light_scale"},
           "Nebula mixed-case fields bind the canonical shader uniform names");
    surface.parameters[2].value = std::numeric_limits<float>::infinity();
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_nonfinite, "nonfinite Nebula distortion blocks");
    surface.parameters[2].value = 2.0F;
    surface.parameters.push_back({"OtherTexture", ParameterKind::texture, std::string("other.tga")});
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::field_invalid, "unreviewed Nebula texture blocks");
    surface.parameters.pop_back();
    surface.vertex_format = "alD3dVertNU2";
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::geometry_invalid, "Nebula requires NU2C vertex colour");
    surface.vertex_format = "alD3dVertNU2C";
    model.bones[0].billboard = 7;
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::hierarchy_unsupported, "placed Nebula billboard stays closed");
    model.bones[0].billboard = 0;
    surface.shader = "Nebula.fxo";
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::shader_unsupported, "Nebula fxo stays closed");
    surface.shader = "Nebula.fx";
    inputs.id.clear();
    expect(space::plan_environment_effect(model, 0, 0, "t0", inputs).status
               == space::EnvironmentEffectStatus::input_invalid, "unnamed external clock and light state blocks");
}


void test_environment_effect_clock() {
    // #185: retail's scene clock gains LogicalFPS / 1000 = 0.03 s per 30 Hz
    // sim frame and wraps at 28800 s; one presentation tick stands for one.
    expect(space::environment_effect_time(0) == 0.0F, "the effect clock starts at zero");
    expect(near(space::environment_effect_time(1), 0.03F), "one tick is 0.03 s of effect time");
    expect(near(space::environment_effect_time(30), 0.9F), "a second of 30 Hz ticks is 0.9 s of effect time");
    expect(near(space::environment_effect_time(59), 1.77F), "the default held capture tick");
    expect(near(space::environment_effect_time(space::effect_clock_wrap_ticks - 1), 28799.97F, 1.0e-2F),
           "the clock runs up to its wrap");
    expect(space::environment_effect_time(space::effect_clock_wrap_ticks) == 0.0F, "the clock wraps at 28800 s");
    expect(space::environment_effect_time(space::effect_clock_wrap_ticks + 30)
               == space::environment_effect_time(30), "a wrapped clock repeats");
    expect(near(static_cast<float>(space::effect_clock_wrap_ticks * space::effect_clock_seconds_per_tick),
                28800.0F), "the wrap is 28800 s");
}


} // namespace eawr_space_test
