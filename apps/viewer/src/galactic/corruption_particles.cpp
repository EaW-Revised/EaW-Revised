#include "corruption_particles.hpp"
#include "eawr/core/diagnostic.hpp"

#include <set>

namespace eawr::presentation::godot_backend {
namespace galactic_detail {
std::optional<std::string> resolve_texture(const vfs::Vfs& filesystem, std::string_view name);
}

CorruptionParticles::CorruptionParticles(godot::Node3D& host, std::shared_ptr<const vfs::Vfs> filesystem)
    : filesystem_(std::move(filesystem)),
      backend_(std::make_unique<GodotParticleBackend>(host,
          [this](std::string_view name) { return texture(name); })),
      registry_(std::make_unique<particles::EffectRegistry>(*backend_)) {
    registry_->set_stream_hashes(false);
}
CorruptionParticles::~CorruptionParticles() = default;

const assets::Texture* CorruptionParticles::texture(std::string_view name) {
    std::string path(name);
    for (char& c : path) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (c == '\\') c = '/';
    }
    auto [entry, inserted] = textures_.try_emplace(path);
    if (inserted) {
        if (const auto resolved = galactic_detail::resolve_texture(*filesystem_, path))
            if (auto loaded = assets::load_texture(*filesystem_, *resolved)) entry->second = std::move(loaded.value());
    }
    return entry->second ? &*entry->second : nullptr;
}

bool CorruptionParticles::service(const viewer::galactic::CorruptionVisuals& visuals,
    const std::string_view type, const particles::SystemDefinition& system,
    const FixedCamera& camera, const double elapsed, std::string& failure) {
    std::set<viewer::galactic::ObjectId> desired;
    for (const auto& attachment : visuals.attachments)
        if (attachment.fact.particle_type == type) desired.insert(attachment.fact.planet);
    for (auto it = live_.begin(); it != live_.end();) {
        const auto attachment = std::find_if(visuals.attachments.begin(), visuals.attachments.end(),
            [&](const auto& fact) { return fact.fact.planet == it->first; });
        if (!desired.contains(it->first) || attachment->generation != it->second.generation) {
            static_cast<void>(registry_->release(it->second.handle)); it = live_.erase(it);
        } else ++it;
    }
    const auto particle_camera = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    particle_count_ = 0;
    for (const auto& attachment : visuals.attachments) {
        if (!desired.contains(attachment.fact.planet)) continue;
        auto found = live_.find(attachment.fact.planet);
        if (found == live_.end()) {
            // A bounded presentation capacity, independent of simulation hashes.
            auto spawned = registry_->spawn(system, ++seed_, 4096);
            if (!spawned) { failure = core::format_diagnostic(spawned.error()); return false; }
            found = live_.emplace(attachment.fact.planet, Live{spawned.value(), attachment.generation}).first;
        }
        particles::EmitterFrame frame;
        frame.origin = {attachment.position.x, attachment.position.y, attachment.position.z};
        const auto posed = registry_->set_frame(found->second.handle, frame);
        if (!posed) { failure = core::format_diagnostic(posed.error()); return false; }
        // U-06: absent level mapping leaves the authored emitter unchanged.
        const auto advanced = registry_->advance(found->second.handle,
            static_cast<float>(std::max(0.0, elapsed)), particle_camera);
        if (!advanced) { failure = core::format_diagnostic(advanced.error()); return false; }
        for (const auto& emitter : advanced.value().emitters) particle_count_ += emitter.particles;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
