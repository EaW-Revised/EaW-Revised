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

void BattleEffects::move_feedback(const sim::math::Vec3& point, const ui::OrderMode mode,
                                  const bool double_click, const std::uint64_t tick) {
    if (released_) return;
    const std::size_t index = mode == ui::OrderMode::attack_move ? 2U
        : mode == ui::OrderMode::guard ? 3U : double_click ? 1U : 0U;
    // OF-03: the double click replaces the nearest ordinary acknowledgement.
    if (index == 1U) {
        auto closest = effects_.end();
        double distance = std::numeric_limits<double>::max();
        for (auto effect = effects_.begin(); effect != effects_.end(); ++effect) {
            if (effect->particle != move_particles_[0]) continue;
            const double dx = effect->frame.origin.x - to_float(point.x);
            const double dy = effect->frame.origin.y - to_float(point.y);
            const double candidate = dx * dx + dy * dy;
            if (candidate < distance) { distance = candidate; closest = effect; }
        }
        if (closest != effects_.end()) {
            static_cast<void>(registry_->release(closest->handle));
            effects_.erase(closest);
        }
    }
    const auto birth = birth_;
    const auto due = due_;
    birth_ = due_ = samples_;
    particles::Basis3 basis;
    // OF-02: the authored model scale affects the emitter frame; the particle
    // size tracks keep their authored dimensions through ordinary projection.
    basis.x.x = basis.y.y = basis.z.z = move_scale_;
    static_cast<void>(spawn(move_particles_[index], {to_float(point.x), to_float(point.y), to_float(point.z)},
        basis, "move_feedback", tick));
    birth_ = birth;
    due_ = due;
}

} // namespace eawr::presentation::godot_backend
