#pragma once

#include "corruption_visuals.hpp"
#include "particle_adapter.hpp"
#include "eawr/vfs/vfs.hpp"

#include <memory>

namespace eawr::presentation::godot_backend {

// WGP-19: the registry and backend live in the midground scene. This owner
// consumes admitted attachment facts; it never queries a simulation owner.
class CorruptionParticles final {
public:
    CorruptionParticles(godot::Node3D& host, std::shared_ptr<const vfs::Vfs> filesystem);
    ~CorruptionParticles();
    [[nodiscard]] bool service(const viewer::galactic::CorruptionVisuals& visuals,
        std::string_view type, const particles::SystemDefinition& system,
        const FixedCamera& camera, double elapsed, std::string& failure);
    [[nodiscard]] std::size_t live_effects() const { return registry_->live_effects(); }
    [[nodiscard]] std::size_t resources() const { return registry_->live_backend_resources(); }
    [[nodiscard]] std::uint64_t particle_count() const { return particle_count_; }
private:
    [[nodiscard]] const assets::Texture* texture(std::string_view name);
    struct Live final { particles::EffectHandle handle{}; std::uint64_t generation{}; };
    std::shared_ptr<const vfs::Vfs> filesystem_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    std::map<viewer::galactic::ObjectId, Live> live_;
    std::uint32_t seed_{};
    std::uint64_t particle_count_{};
};

} // namespace eawr::presentation::godot_backend
