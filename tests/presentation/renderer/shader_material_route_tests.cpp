#include "eawr/presentation/shader_material_route.hpp"
#include "eawr/presentation/shader_material_activation.hpp"
#include "../../support/check.hpp"
#include <bit>
#include <cmath>
#include <iostream>
#include <cstdlib>
#include <new>
#include <limits>
#include <sstream>

namespace {
std::size_t hot_allocations{};
bool track_hot_allocations{};
}
void* operator new(std::size_t bytes) {
    if (track_hot_allocations) ++hot_allocations;
    if (void* memory = std::malloc(bytes ? bytes : 1U)) return memory;
    throw std::bad_alloc();
}
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

namespace {
using namespace eawr;
namespace material = presentation::shader_material;
namespace route = material::routing;
namespace activation = presentation::shader_activation;
namespace evaluation = presentation::shader_evaluation;
testing::Checker checks;
void check(bool value, std::string_view message) { checks.check(value, message, std::cerr); }
std::shared_ptr<activation::Generation> generation(bool time = false, bool texture = false) {
    auto resident = std::make_shared<material::Resident>();
    auto& program = resident->program;
    program.material.route = presentation::MaterialRoute::modern_spatial;
    program.material.bindings.push_back({"numeric", assets::Vec4f{}});
    const presentation::shader_bindings::NumericShape shape{assets::EffectValueKind::floating,
        time ? assets::EffectValueCategory::scalar : assets::EffectValueCategory::vector, 1U, time ? 1U : 4U, 1U};
    if (!time) program.parameters.push_back({3U, "Tint", shape});
    evaluation::Uniform uniform;
    uniform.binding.name = time ? "Clock" : "Tint";
    uniform.binding.shape = shape;
    uniform.binding.register_set = assets::EffectRegisterSet::floating4;
    uniform.binding.register_count = 1U;
    uniform.binding.source = presentation::shader_bindings::ConstantSource::effect_parameter;
    uniform.binding.parameter = 3U;
    uniform.operand.kind = assets::EffectValueKind::floating;
    uniform.operand.parameters.push_back({3U, {}});
    if (time) {
        uniform.binding.engine_input = presentation::shader_bindings::EngineInput::time;
        uniform.operand.parameters[0U].engine_input = uniform.binding.engine_input;
    } else uniform.operand.defaults.assign(4U, std::bit_cast<std::uint32_t>(1.0F));
    uniform.words = 4U;
    program.vertex.updates.words = 4U;
    program.vertex.updates.uniforms.push_back(std::move(uniform));
    program.vertex.uniforms.push_back({"numeric", {presentation::shader_lowering::Scalar::floating, 4U}, 0U, 0U, false});
    if (texture) {
        program.textures.push_back({"sampled", "Image", "authored", {}});
        program.material.bindings.push_back({"sampled", std::string("authored")});
    }
    auto result = std::make_shared<activation::Generation>(); result->id = 1U;
    activation::EffectResult effect; effect.input.effect = "data/art/shaders/engine/invented";
    effect.variants.push_back({{}, presentation::shader_report::Route::translated, std::move(resident), {}});
    result->effects.push_back(std::move(effect)); return result;
}
void controls() {
    check(route::recognized_pci_vendor("NVIDIA Corporation") == 0x10deU
        && route::recognized_pci_vendor("AMD") == 0x1002U
        && route::recognized_pci_vendor("Intel") == 0x8086U,
        "recognized actual driver names preserve vendor-filter PCI identity");
    check(!route::recognized_pci_vendor("Unknown") && !route::recognized_pci_vendor("Mesa/X.org")
        && !route::recognized_pci_vendor("nvidia") && !route::recognized_pci_vendor(""),
        "unknown or guessed vendor strings cannot become vendor zero or bypass a filter");
    material::Program provider_policy;
    check(route::provider_refusal(provider_policy, false) == route::ProviderRefusal::none
        && route::provider_refusal(provider_policy, true) == route::ProviderRefusal::fog_variant,
        "fog-enabled contexts cannot silently draw a native surface without their fog variant");
    check(route::provider_refusal(provider_policy, false, true) == route::ProviderRefusal::skinning,
        "palette geometry requires qualified source skinning order even when a program reads no palette uniform");
    provider_policy.parameters.push_back({0U, "Colorization", {}});
    check(route::provider_refusal(provider_policy, false) == route::ProviderRefusal::colorization,
        "unqualified per-entity team-colour integration selects the explicit existing fallback");
    provider_policy.parameters.clear(); provider_policy.extended_vertex_inputs = true;
    check(route::provider_refusal(provider_policy, false) == route::ProviderRefusal::vertex_attributes,
        "unqualified raw vertex channels cannot silently use normalized, packed or missing renderer attributes");
    std::ostringstream report;
    report << "{\n  \"scene\":{},\n";
    const auto before = report.str();
    route::write_route_report(report, false, 9U, 8U, 7U, 6U);
    check(report.str() == before, "activation OFF report hook appends zero bytes even with retained prior counters");
    route::write_route_report(report, true, 9U, 8U, 7U, 6U);
    check(report.str() == before + "  \"shader_translation\":{\"selection_policy\":\"desktop-ST-O1\",\"uploaded\":9,\"approximated\":8,\"refused\":7,\"missing_green\":6},\n",
        "enabled report distinguishes actual native, adapter approximation, refusal and missing-green counts");
    const auto enabled = report.str();
    route::write_route_report(report, false, 9U, 8U, 7U, 6U);
    check(report.str() == enabled, "switching report activation OFF preserves existing bytes");
    std::cout << "shader route inline bytes: surface=" << sizeof(route::Surface)
        << " override=" << sizeof(route::NumericOverride)
        << " input=" << sizeof(evaluation::ParameterInput)
        << " texture-request=" << sizeof(route::TextureRequest)
        << " generation-pin=" << sizeof(std::shared_ptr<const activation::Generation>) << '\n';
    presentation::FixedCamera camera;
    camera.width = 1600U; camera.height = 800U;
    camera.near_plane = 2.0F; camera.far_plane = 10.0F; camera.vertical_fov_degrees = 90.0F;
    const auto projection = route::source_projection(camera);
    check(bool(projection), "finite current camera supplies the source projection");
    if (projection) {
        const auto close = [](float left, float right) { return std::abs(left - right) < 0.00001F; };
        const auto depth = [&](float z) { return ((*projection)[10U] * z + (*projection)[14U]) / z; };
        check(close(depth(2.0F), 0.0F) && close(depth(10.0F), 1.0F) && close(depth(4.0F), 0.625F)
            && close((*projection)[0U], 0.5F) && close((*projection)[5U], 1.0F),
            "source projection retains D3D near/middle/far depth and vertical-FOV aspect");
        route::Matrix world{};
        world[0U] = world[5U] = world[10U] = world[15U] = 1.0F; world[14U] = 4.0F;
        const auto combined = route::multiply(*projection, world);
        check(close(combined[14U], 2.5F) && close(combined[15U], 4.0F),
            "column-major projection times world preserves the source row-vector transpose convention");
    }
    camera.near_plane = 0.0F;
    check(!route::source_projection(camera), "invalid near plane cannot supply a guessed projection");
    camera.near_plane = 2.0F; camera.far_plane = std::numeric_limits<float>::infinity();
    check(!route::source_projection(camera), "nonfinite camera projection refuses its provider");
    std::size_t resolutions{};
    const auto resolve = [&](std::string_view declared) -> std::optional<std::string> {
        ++resolutions;
        if (declared == "authored" || declared == "replacement") return "data/art/textures/" + std::string(declared) + ".dds";
        return {};
    };
    auto off = route::bind({}, "invalid:/shader", {}, {}, resolve);
    check(off && !off.value() && resolutions == 0U, "default-off route performs no identity, resource or compiler work");
    const auto identity = route::effect_identity("Engine\\Invented.FXO");
    check(identity && identity.value() == "data/art/shaders/engine/invented", "route identity preserves canonical shader subdirectories");
    auto owner = generation(false, true);
    auto late = route::bind(owner, "late.fx", {}, {}, resolve);
    check(late && !late.value() && resolutions == 0U, "unknown battle-time effects keep the existing fallback without work");
    const std::array parameters{assets::MaterialParameter{"Tint", assets::ParameterKind::vector4, assets::Vec4f{2, 3, 4, 5}},
        assets::MaterialParameter{"Image", assets::ParameterKind::texture, std::string("replacement")}};
    auto bound = route::bind(owner, "Engine/Invented.fx", parameters, {}, resolve);
    const std::array runtime_override{presentation::MaterialBinding{"Tint", assets::Vec4f{11, 12, 13, 14}}};
    auto overridden = route::bind(owner, "Engine/Invented.fx", parameters, {}, resolve, {}, runtime_override);
    check(overridden && overridden.value(), "typed current material override admits without changing source parameters");
    if (overridden && overridden.value()) {
        const auto* value = std::get_if<assets::Vec4f>(&overridden.value()->instance.material.bindings[0U].value);
        check(value && value->x == 11.0F && value->w == 14.0F,
            "current team/material parameter override reaches the actual emitted source register");
    }
    const std::array repeated_override{runtime_override[0U], runtime_override[0U]};
    check(!route::bind(owner, "Engine/Invented.fx", parameters, {}, resolve, {}, repeated_override),
        "duplicate current material overrides cannot choose an incidental winner");
    check(bound && bound.value(), "admitted program binds authored numeric and texture overrides");
    if (bound && bound.value()) {
        const auto* tint = std::get_if<assets::Vec4f>(&bound.value()->instance.material.bindings[0U].value);
        check(tint && tint->x == 2.0F && tint->w == 5.0F && bound.value()->textures[0U].resource.ends_with("replacement.dds"),
            "numeric values and the actual texture parameter reach the translated material");
        const auto* words = bound.value()->instance.vertex.words.data();
        route::Surface moved(std::move(*bound.value()));
        check(bool(moved.update({})) && moved.instance.vertex.words.data() == words,
            "moving and updating a prepared surface preserves owned input/output buffers");
    }
    const std::array wrong_shape{assets::MaterialParameter{"Tint", assets::ParameterKind::vector3, assets::Vec3f{1, 2, 3}}};
    check(!route::bind(owner, "Engine/Invented.fx", wrong_shape, {}, resolve), "numeric shape mismatches select an explicit fallback");
    const std::array duplicates{parameters[0U], parameters[0U]};
    check(!route::bind(owner, "Engine/Invented.fx", duplicates, {}, resolve), "duplicate used parameters cannot choose an incidental winner");
    const std::array absent{assets::MaterialParameter{"Image", assets::ParameterKind::texture, std::string("absent")}};
    check(!route::bind(owner, "Engine/Invented.fx", absent, {}, resolve), "missing authored resources cannot acquire a visible replacement");
    auto clock_owner = generation(true);
    const auto before_missing_provider = resolutions;
    check(!route::bind(generation(true, true), "Engine/Invented.fx", {}, {}, resolve)
        && resolutions == before_missing_provider,
        "missing numeric providers refuse before effect texture decoding");
    check(!route::bind(clock_owner, "Engine/Invented.fx", {}, {}, resolve), "current engine inputs are mandatory at surface preparation");
    const std::array stale_clock{assets::MaterialParameter{"Clock", assets::ParameterKind::scalar, 6.0F}};
    check(!route::bind(clock_owner, "Engine/Invented.fx", stale_clock, {}, resolve),
        "an authored field cannot substitute for a missing engine-owned clock provider");
    const std::array runtime_clock{presentation::MaterialBinding{"Clock", 6.0F}};
    check(!route::bind(clock_owner, "Engine/Invented.fx", {}, {}, resolve, {}, runtime_clock),
        "a current material override cannot substitute for an engine-owned clock provider");
    std::array clock_words{std::bit_cast<std::uint32_t>(7.0F)};
    const std::array clock_inputs{evaluation::EngineInput{presentation::shader_bindings::EngineInput::time,
        {assets::EffectValueKind::floating, clock_words}}};
    auto clock = route::bind(clock_owner, "Engine/Invented.fx", {}, clock_inputs, resolve);
    check(clock && clock.value(), "current scene provider admits the prepared surface");
    if (clock && clock.value()) {
        clock_words[0U] = std::bit_cast<std::uint32_t>(9.0F);
        check(bool(clock.value()->update(clock_inputs)), "live provider updates remain outside the compiler route");
        const auto* value = std::get_if<assets::Vec4f>(&clock.value()->instance.material.bindings[0U].value);
        check(value && value->x == 9.0F, "updated current input reaches its exact register");
        bool all_valid = true;
        const auto before_allocations = hot_allocations;
        track_hot_allocations = true;
        for (std::size_t frame = 0U; frame < 64U; ++frame) {
            clock_words[0U] = std::bit_cast<std::uint32_t>(static_cast<float>(frame));
            all_valid = bool(clock.value()->update(clock_inputs)) && all_valid;
        }
        track_hot_allocations = false;
        check(all_valid && hot_allocations == before_allocations,
            "repeated live numeric updates perform zero heap allocations");
        check(!clock.value()->update({}) && !clock.value()->instance.valid, "missing later input invalidates the complete surface");
    }
    auto retained_owner = generation(); std::weak_ptr<activation::Generation> weak = retained_owner;
    auto retained = route::bind(retained_owner, "Engine/Invented.fx", {}, {}, resolve); retained_owner.reset();
    check(retained && retained.value() && !weak.expired(), "a surface pins the admitted mod generation across switching");
    if (retained) retained.value().reset();
    check(weak.expired(), "last surface release drops its old generation pin");
}
} // namespace
int main() { controls(); return checks.finish(); }
