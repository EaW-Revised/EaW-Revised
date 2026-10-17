#include "corruption_particles.hpp"
#include "eawr/core/diagnostic.hpp"

#include <cmath>

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
    const data::Catalog& catalog, const viewer::galactic::SceneContent& content, const std::size_t layer,
    const FixedCamera& camera, const double elapsed, std::string& failure) {
    budget_.begin_frame();
    desired_.clear();
    for (const auto& attachment : visuals.attachments) {
        auto [entry, inserted] = layers_.try_emplace(attachment.fact.particle_type);
        if (inserted) entry->second = static_cast<std::size_t>(viewer::galactic::galactic_particle_layer(catalog, attachment.fact.particle_type));
        if (entry->second == layer) desired_.emplace_back(attachment.fact.planet, &attachment);
    }
    std::sort(desired_.begin(), desired_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto it = live_.begin(); it != live_.end();) {
        const auto attachment = std::lower_bound(desired_.begin(), desired_.end(), it->first,
            [](const auto& pair, const auto id) { return pair.first < id; });
        if (attachment == desired_.end() || attachment->first != it->first || attachment->second->generation != it->second.generation) {
            const auto released = budget_.release(*registry_, it->second.handle);
            if (!released) { failure = core::format_diagnostic(released.error()); return false; }
            it = live_.erase(it);
        } else ++it;
    }
    const auto particle_camera = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    particle_count_ = 0;
    for (const auto& [id, desired] : desired_) {
        static_cast<void>(id);
        const auto& attachment = *desired;
        auto found = live_.find(attachment.fact.planet);
        if (found == live_.end()) {
            // A bounded presentation capacity, independent of simulation hashes.
            auto [definition, inserted] = systems_.try_emplace(attachment.fact.particle_type);
            if (inserted) {
                if (attachment.fact.particle_type == content.corruption_particle) definition->second = content.corruption_system;
                else if (attachment.fact.particle_type == content.corruption_style.line_particle) definition->second = content.corruption_line_system;
                else definition->second = viewer::galactic::load_galactic_particle(*filesystem_, catalog, attachment.fact.particle_type);
            }
            if (!definition->second) continue;
            auto spawned = budget_.spawn(*registry_, *definition->second, ++seed_);
            if (!spawned) { failure = core::format_diagnostic(spawned.error()); return false; }
            if (!spawned.value()) continue;
            found = live_.emplace(attachment.fact.planet, Live{*spawned.value(), attachment.generation}).first;
        }
        particles::EmitterFrame frame;
        frame.origin = {attachment.position.x, attachment.position.y, attachment.position.z};
        const auto posed = registry_->set_frame(found->second.handle, frame);
        if (!posed) { failure = core::format_diagnostic(posed.error()); return false; }
        const auto brightness = registry_->set_brightness(found->second.handle, attachment.fact.brightness.value_or(1));
        if (!brightness) { failure = core::format_diagnostic(brightness.error()); return false; }
        const auto dynamic = registry_->set_dynamic(found->second.handle,
            attachment.fact.scale.value_or(1), attachment.fact.energy.value_or(1));
        if (!dynamic) { failure = core::format_diagnostic(dynamic.error()); return false; }
        const auto advanced = registry_->advance(found->second.handle,
            static_cast<float>(std::max(0.0, elapsed)), particle_camera);
        if (!advanced) { failure = core::format_diagnostic(advanced.error()); return false; }
        for (const auto& emitter : advanced.value().emitters) particle_count_ += emitter.particles;
    }
    return true;
}

bool CorruptionParticles::service_lines(const viewer::galactic::CorruptionVisuals& visuals,
    const viewer::galactic::GalaxyView& view, const viewer::galactic::CorruptionStyle& style,
    const particles::SystemDefinition& system, const std::uint64_t logical_frame,
    const FixedCamera& camera, const double elapsed, std::string& failure) {
    if (std::none_of(system.emitters.begin(), system.emitters.end(), [](const auto& emitter) { return emitter.cpu_ready; })) return true;
    budget_.begin_frame();
    desired_lines_.clear();
    for (const auto& connection : visuals.connections) if (!connection.path) desired_lines_.push_back(connection.generation);
    std::sort(desired_lines_.begin(), desired_lines_.end());
    for (auto it = lines_.begin(); it != lines_.end();) {
        if (!std::binary_search(desired_lines_.begin(), desired_lines_.end(), it->first)) {
            const auto released = budget_.release(*registry_, it->second.handle);
            if (!released) { failure = core::format_diagnostic(released.error()); return false; }
            it = lines_.erase(it);
        } else ++it;
    }
    particle_count_ = 0;
    const auto particle_camera = particles::camera_frame_from_render(camera.eye, camera.target, camera.up);
    planets_.clear();
    for (const auto& planet : view.planets) planets_.emplace_back(planet.id, &planet);
    std::sort(planets_.begin(), planets_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& connection : visuals.connections) {
        if (connection.path) continue;
        auto found = lines_.find(connection.generation);
        const auto lookup = [&](const auto id) {
            return std::lower_bound(planets_.begin(), planets_.end(), id, [](const auto& pair, const auto key) { return pair.first < key; });
        };
        const auto a = lookup(connection.from), b = lookup(connection.to);
        if (a == planets_.end() || a->first != connection.from || b == planets_.end() || b->first != connection.to) continue;
        const auto geometry = viewer::galactic::corruption_line_geometry(a->second->position, b->second->position, style);
        if (!geometry || style.grow_seconds <= 0) continue;
        if (found == lines_.end()) {
            auto spawned = budget_.spawn(*registry_, system, ++seed_);
            if (!spawned) { failure = core::format_diagnostic(spawned.error()); return false; }
            if (!spawned.value()) continue;
            Line line; line.handle = *spawned.value();
            found = lines_.emplace(connection.generation, std::move(line)).first;
        }
        auto& line = found->second;
        const auto convert = [](viewer::galactic::Vec3 value) { return particles::Vec3{value.x,value.y,value.z}; };
        particles::EmitterFrame frame;
        frame.origin = convert(geometry->origin);
        frame.basis = {convert(geometry->x), convert(geometry->y), convert(geometry->z)};
        const auto posed = registry_->set_frame(line.handle, frame);
        if (!posed) { failure = core::format_diagnostic(posed.error()); return false; }
        const auto volume = registry_->set_pinched_cylinder(line.handle, style.radius, geometry->height,
            line.growth.service(logical_frame, style.grow_seconds));
        if (!volume) { failure = core::format_diagnostic(volume.error()); return false; }
        const auto advanced = registry_->advance(line.handle, static_cast<float>(std::max(0.0, elapsed)), particle_camera);
        if (!advanced) { failure = core::format_diagnostic(advanced.error()); return false; }
        for (const auto& emitter : advanced.value().emitters) particle_count_ += emitter.particles;
    }
    return true;
}

} // namespace eawr::presentation::godot_backend
