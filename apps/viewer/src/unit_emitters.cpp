#include "unit_emitters.hpp"

#include "eawr/presentation/animation/animation.hpp"
#include "eawr/presentation/particles/map_attachment_owner.hpp"
#include "eawr/presentation/particles/map_effect_plan.hpp"
#include "eawr/presentation/particles/prewarmed_capacity.hpp"
#include "eawr/presentation/particles/proxy_binding.hpp"
#include "eawr/presentation/space/debris.hpp"
#include "eawr/presentation/space/live_units.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <utility>

#include "unit_emitters_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace unit_emitters_detail;


UnitEmitters::UnitEmitters(godot::Node3D& host, const vfs::Vfs& filesystem)
    : host_(&host), filesystem_(&filesystem), cache_(filesystem),
      backend_(std::make_unique<GodotParticleBackend>(
          host, [this](const std::string_view name) { return resolve_texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {
    // #638: nothing here reads the streams' hashes, so the frames do not compute them.
    registry_->set_stream_hashes(false);
}

UnitEmitters::~UnitEmitters() { release(); }

void UnitEmitters::follow_lighting(const GodotRenderer& renderer) {
    const auto& lighting = renderer.lighting();
    if (!lighting) return;
    // R-LIT-01/R-LIT-04: the backend caches this state and updates bump
    // materials only when the authored scene lighting changes.
    backend_->set_lighting({.toward_light = lighting->toward_light, .diffuse = lighting->sun_diffuse,
        .specular = lighting->specular, .fill = lighting->sph_fill});
}

} // namespace eawr::presentation::godot_backend
