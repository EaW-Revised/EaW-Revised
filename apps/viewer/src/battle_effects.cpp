#include "eawr/core/load_profile.hpp"
#include "battle_effects.hpp"

#include "eawr/presentation/space/space.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/start.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "battle_effects_internal.hpp"

namespace eawr::presentation::godot_backend {
using namespace godot;
namespace tactical = sim::tactical;
using namespace battle_effects_detail;


BattleEffects::BattleEffects(godot::Node3D& host, const vfs::Vfs& filesystem, const data::Catalog& catalog)
    : host_(&host), filesystem_(&filesystem), catalog_(&catalog),
      backend_(std::make_unique<GodotParticleBackend>(
          host, [this](const std::string_view name) { return resolve_texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {
    // #638: nothing here reads the streams' hashes, so the frames do not compute them.
    registry_->set_stream_hashes(false);
}

BattleEffects::~BattleEffects() { release(); }

} // namespace eawr::presentation::godot_backend
