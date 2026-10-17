#pragma once

#include "corruption_visuals.hpp"
#include "corruption_budget.hpp"
#include "particle_adapter.hpp"
#include "scene_content.hpp"
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
        const data::Catalog& catalog, const viewer::galactic::SceneContent& content, std::size_t layer,
        const FixedCamera& camera, double elapsed, std::string& failure);
    [[nodiscard]] bool service_lines(const viewer::galactic::CorruptionVisuals& visuals,
        const viewer::galactic::GalaxyView& view, const viewer::galactic::CorruptionStyle& style,
        const particles::SystemDefinition& system, std::uint64_t logical_frame,
        const FixedCamera& camera, double elapsed, std::string& failure);
    [[nodiscard]] std::size_t live_effects() const { return registry_->live_effects(); }
    [[nodiscard]] std::size_t resources() const { return registry_->live_backend_resources(); }
    [[nodiscard]] std::uint64_t particle_count() const { return particle_count_; }
    [[nodiscard]] std::size_t reserved_particles() const { return budget_.used(); }
    [[nodiscard]] std::size_t refused_effects() const { return budget_.refused(); }
private:
    [[nodiscard]] const assets::Texture* texture(std::string_view name);
    struct Live final { particles::EffectHandle handle{}; std::uint64_t generation{}; };
    struct Line final {
        particles::EffectHandle handle{};
        viewer::galactic::CorruptionLineGrowth growth;
    };
    std::shared_ptr<const vfs::Vfs> filesystem_;
    std::map<std::string, std::optional<assets::Texture>, std::less<>> textures_;
    std::unique_ptr<GodotParticleBackend> backend_;
    std::unique_ptr<particles::EffectRegistry> registry_;
    std::map<viewer::galactic::ObjectId, Live> live_;
    std::map<std::uint64_t, Line> lines_;
    std::map<std::string, std::optional<particles::SystemDefinition>, std::less<>> systems_;
    std::map<std::string, std::size_t, std::less<>> layers_;
    std::uint32_t seed_{};
    std::uint64_t particle_count_{};
    viewer::galactic::CorruptionParticleBudget budget_;
    std::vector<std::pair<viewer::galactic::ObjectId, const viewer::galactic::CorruptionAttachment*>> desired_;
    std::vector<std::uint64_t> desired_lines_;
    std::vector<std::pair<viewer::galactic::ObjectId, const viewer::galactic::PlanetView*>> planets_;
};

} // namespace eawr::presentation::godot_backend
