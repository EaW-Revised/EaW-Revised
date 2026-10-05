#include "render_internal.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utility>

// Quad construction follows the MIT-licensed alo-viewer revision
// 9bb0053919cc5df8377610d4f91b11d956d6c2f4 (DirectX9/ParticleRenderers.cpp):
// corner order, the 0-1-2 / 2-1-3 index pattern, the texture-coordinate
// assignment. Legacy kite geometry follows MD-07. Blend, depth-write and phase policy
// are taken from the render state of the public Engine/Prim* effects that the
// legacy selector names; see docs/reports/P1-08-rendering.md.

namespace eawr::presentation::particles {
using namespace render_detail;

namespace {
// Legacy V1 selector table. Program names are the public engine effects the
// selector indexes; techniques are the first active technique in each source.
constexpr std::array<LegacyBlendSelector, 14> selectors{{
    {0, "Engine/PrimOpaque.fx", "t0", true, ""},
    {1, "Engine/PrimAdditive.fx", "t1", true, ""},
    {2, "Engine/PrimAlpha.fx", "t1", true, ""},
    {3, "Engine/PrimModulate.fx", "t0", true, ""},
    {4, "Engine/PrimDepthSpriteAdditive.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {5, "Engine/PrimDepthSpriteAlpha.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {6, "Engine/PrimDepthSpriteModulate.fx", "Depth_Sprite_PS20", false,
     "depth sprite replaces per-pixel depth from the emitter's depth texture; not implemented"},
    {7, "Engine/PrimDiffuseAlpha.fx", "t1", false,
     "lit 2x diffuse primitive needs the #25 lighting interface"},
    {8, "Engine/StencilDarken.fx", "", false, "stencil darkening pipeline is not implemented"},
    {9, "Engine/StencilDarkenFinalBlur.fx", "", false,
     "stencil darkening blur pipeline is not implemented"},
    {10, "Engine/PrimHeat.fx", "t2", true, ""},
    {11, "Engine/PrimParticleBumpAlpha.fx", "t0", true, ""},
    {12, "Engine/PrimDecalBumpAlpha.fx", "", false,
     "bump-mapped decal lighting needs the #25 lighting interface"},
    {13, "Engine/PrimAlphaScanlines.fx", "t0", false,
     "scanline pattern shading is not implemented"},
}};
} // namespace

std::string_view to_string(const RenderFamily family) noexcept {
    switch (family) {
    case RenderFamily::billboard: return "billboard";
    case RenderFamily::xy_aligned: return "xy_aligned";
    case RenderFamily::heat_saturation: return "heat_saturation";
    case RenderFamily::kites: return "kites";
    case RenderFamily::unsupported: return "unsupported";
    }
    return "unsupported";
}

std::string_view to_string(const Blend blend) noexcept {
    switch (blend) {
    case Blend::opaque: return "opaque";
    case Blend::additive: return "additive";
    case Blend::alpha: return "alpha";
    case Blend::modulate: return "modulate";
    case Blend::heat_distortion: return "heat_distortion";
    case Blend::bump_alpha: return "bump_alpha";
    }
    return "invalid";
}

std::string_view to_string(const DrawPhase phase) noexcept {
    switch (phase) {
    case DrawPhase::opaque: return "opaque";
    case DrawPhase::transparent: return "transparent";
    case DrawPhase::heat: return "heat";
    }
    return "invalid";
}


std::span<const LegacyBlendSelector> legacy_blend_selectors() noexcept { return selectors; }

EmitterRenderPlan plan_emitter(const EmitterDefinition& emitter, const std::size_t index) {
    EmitterRenderPlan plan;
    plan.emitter_index = index;
    plan.renderer_id = emitter.renderer_id;
    plan.blend_selector = emitter.blend_mode;
    plan.texture = emitter.color_texture;
    plan.tail_size = emitter.tail_size;
    plan.legacy_kite_motion = emitter.legacy_kite_motion;
    plan.inherit_emitter_motion = emitter.inherit_emitter_motion;
    const auto& velocity = emitter.velocity;
    Vec3 maximum = velocity.point;
    if (velocity.shape == Shape::range) {
        maximum = {std::max(std::fabs(velocity.range_min.x), std::fabs(velocity.range_max.x)),
                   std::max(std::fabs(velocity.range_min.y), std::fabs(velocity.range_max.y)),
                   std::max(std::fabs(velocity.range_min.z), std::fabs(velocity.range_max.z))};
    } else if (velocity.shape == Shape::sphere) {
        maximum = {velocity.radius_max, 0, 0};
    } else if (velocity.shape == Shape::cylinder) {
        maximum = {velocity.cylinder_radius, 0,
                   std::max(std::fabs(velocity.cylinder_height_min), std::fabs(velocity.cylinder_height_max))};
    }
    plan.kite_speed_limit = length(maximum) + length(emitter.acceleration);
    plan.depth_test = !emitter.disable_depth_test;
    plan.triangles = emitter.primitive_mode == 0;
    plan.sort_particles = emitter.depth_sort && plan.depth_test
        && (emitter.blend_mode == 0 || emitter.blend_mode == 2
            || emitter.blend_mode == 5 || emitter.blend_mode == 11)
        && emitter.renderer_id != 38;
    plan.order_in_phase = static_cast<std::uint32_t>(std::min<std::size_t>(index, 0xffffffffU));
    const auto fail = [&](std::string cause) {
        plan.drawable = false;
        plan.cause = std::move(cause);
        return plan;
    };
    if (emitter.primitive_mode > 1) return fail("unknown legacy primitive selector");

    switch (emitter.renderer_id) {
    case 22: plan.family = RenderFamily::billboard; break;
    case 28: plan.family = RenderFamily::xy_aligned; break;
    case 38: plan.family = RenderFamily::heat_saturation; break;
    case 52: plan.family = RenderFamily::kites; break;
    default: {
        const PluginInfo* info = find_plugin(emitter.renderer_id);
        return fail("renderer family " + std::string(info ? info->name : "unknown")
            + " has no adapter; the CPU runtime produces no data for it");
    }
    }
    if (plan.triangles && plan.family == RenderFamily::kites)
        return fail("triangle kite geometry has no verified adapter");

    if (emitter.blend_mode >= selectors.size()) {
        return fail("legacy blend selector " + std::to_string(emitter.blend_mode)
            + " is outside the fourteen-entry engine effect table");
    }
    const LegacyBlendSelector& selector = selectors[emitter.blend_mode];
    plan.program = selector.program;
    plan.technique = selector.technique;
    if (!emitter.cpu_ready) {
        return fail("emitter is not CPU-ready: " + emitter.unsupported_reason);
    }
    const bool heat_selector = emitter.blend_mode == 10;
    const bool heat_family = plan.family == RenderFamily::heat_saturation;
    // Every corpus heat emitter carries selector 1 or 2 with a distortion
    // texture (signed red/green offset, alpha mask), so the heat renderer draws
    // it as screen distortion, the only public heat-phase program. The authored
    // selector stays in blend_selector.
    if (heat_family && !heat_selector && emitter.blend_mode != 1 && emitter.blend_mode != 2) {
        return fail("heat renderer with selector " + std::to_string(emitter.blend_mode)
            + " has no derived distortion policy");
    }
    if (!heat_family && heat_selector) {
        return fail("PrimHeat selector outside the heat renderer has no derived distortion source");
    }
    if (!selector.supported) return fail(std::string(selector.cause));
    if (emitter.color_texture.empty()) return fail("emitter declares no colour texture");
    if (heat_family) {
        const LegacyBlendSelector& heat = selectors[10];
        plan.program = heat.program;
        plan.technique = heat.technique;
        plan.blend = Blend::heat_distortion;
        plan.phase = DrawPhase::heat;
        plan.drawable = true;
        return plan;
    }

    switch (emitter.blend_mode) {
    case 0: plan.blend = Blend::opaque; plan.phase = DrawPhase::opaque; plan.depth_write = true; break;
    case 1: plan.blend = Blend::additive; break;
    case 2: plan.blend = Blend::alpha; break;
    case 3: plan.blend = Blend::modulate; break;
    case 10: plan.blend = Blend::heat_distortion; plan.phase = DrawPhase::heat; break;
    case 11: plan.blend = Blend::bump_alpha; plan.normal_texture = emitter.normal_texture; break;
    default: return fail("selector has no blend policy");
    }
    plan.drawable = true;
    return plan;
}

std::vector<EmitterRenderPlan> plan_system(const SystemDefinition& system) {
    std::vector<EmitterRenderPlan> plans;
    plans.reserve(system.emitters.size());
    for (std::size_t index = 0; index < system.emitters.size(); ++index) {
        plans.push_back(plan_emitter(system.emitters[index], index));
    }
    return plans;
}


} // namespace eawr::presentation::particles
