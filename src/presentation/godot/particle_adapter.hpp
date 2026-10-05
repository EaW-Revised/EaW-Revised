#pragma once

#include "eawr/assets/assets.hpp"
#include "eawr/presentation/particles/render.hpp"
#include "eawr/presentation/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace godot {
class Node3D;
class RID;
}

namespace eawr::presentation::godot_backend {

// RenderingServer backend for the engine-free particle registry. Each drawable
// emitter owns one mesh, one material and one instance in the host's scenario;
// textures and shaders are shared and reference counted by name/policy. Vertex
// streams arrive in the ALO basis and receive the one documented conversion
// (x, y, z) -> (x, z, -y) here, exactly as model vertices do in renderer.cpp.
class GodotParticleBackend final : public particles::RenderBackend {
public:
    // Returns the decoded colour texture for an authored name, or nullptr when
    // it does not resolve. The backend never names a path or opens a file.
    using TextureResolver = std::function<const assets::Texture*(std::string_view name)>;
    struct FogCallbacks final {
        std::function<core::Result<std::uint64_t>(const godot::RID&, const godot::RID&)> register_material;
        std::function<void(std::uint64_t)> unregister_material;
        // Samples the final transformed ALO world XY of each submitted vertex.
        std::function<std::uint8_t(float, float)> attenuation_at_source_xy;
    };
    struct FogEmitterEvidence final {
        std::uint64_t resource{};
        std::uint64_t fog_handle{};
        std::size_t emitter_index{};
        std::size_t quads{};
        std::uint8_t minimum_attenuation{255};
        std::uint8_t maximum_attenuation{};
    };

    GodotParticleBackend(godot::Node3D& host, TextureResolver resolver, FogCallbacks fog = {});
    ~GodotParticleBackend() override;
    GodotParticleBackend(const GodotParticleBackend&) = delete;
    GodotParticleBackend& operator=(const GodotParticleBackend&) = delete;

    [[nodiscard]] std::uint64_t create_emitter(const particles::EmitterRenderPlan& plan) override;
    void update_emitter(std::uint64_t resource, const particles::VertexStream& stream) override;
    void destroy_emitter(std::uint64_t resource) override;
    [[nodiscard]] std::string failure_cause() const override;
    [[nodiscard]] particles::CullingFrame culling_frame() const override;
    void set_emitter_visible(std::uint64_t resource, bool visible) override;
    void record_group_work(bool visible, bool prepared, bool stepped) override;
    // The scene light of bump-mapped emitters (PrimParticleBumpAlpha): light
    // 0's toward-light vector in the render (Y-up) world basis, its diffuse
    // and specular colours, and the SPH_LIGHT_FILL irradiance matrices in the
    // render basis (column-major, as GodotRenderer::LightingState::sph_fill).
    // Without a call the renderer's hemisphere fallback is used.
    struct BumpLighting final {
        std::array<float, 3> toward_light{};
        std::array<float, 3> diffuse{};
        std::array<float, 3> specular{};
        std::array<std::array<float, 16>, 3> fill{};
        friend bool operator==(const BumpLighting&, const BumpLighting&) = default;
    };
    [[nodiscard]] static BumpLighting default_bump_lighting() noexcept;
    // Applies to live and later emitters; repeating the current state is free.
    void set_lighting(const BumpLighting& lighting);

    // The material each emitter was created with. Particle materials pass the
    // same versioned MaterialDescription gate as every other renderer consumer.
    [[nodiscard]] static MaterialDescription material_for(const particles::EmitterRenderPlan& plan,
        bool fog = false);
    [[nodiscard]] static std::string_view adapter_name(particles::RenderFamily family) noexcept;
    [[nodiscard]] static std::string_view material_adapter_name(particles::Blend blend) noexcept;
    // PrimHeat's DistortionAmount: a heat texel offsets its scene sample by at
    // most this fraction of the screen, times texture alpha and vertex alpha.
    static constexpr float heat_distortion_amount = 0.01F;
    [[nodiscard]] static RenderPass pass_for(particles::DrawPhase phase) noexcept;

    // Every RenderingServer RID this backend currently owns (meshes, materials,
    // instances, shared textures and shaders). Zero after the last emitter is
    // destroyed is the no-leak evidence.
    [[nodiscard]] std::size_t live_rids() const noexcept;
    [[nodiscard]] std::size_t live_emitters() const noexcept;
    [[nodiscard]] std::vector<FogEmitterEvidence> fog_emitter_evidence() const;

    // Opt-in frame profiling across the main thread's particle backends. Uploads include
    // only nonempty streams; replacements count newly allocated mesh surfaces.
    struct FrameWork final {
        std::uint64_t streams{};
        std::uint64_t uploads{};
        std::uint64_t replacements{};
        double conversion_ms{};
        double submission_ms{};
        std::uint64_t groups{}, visible_groups{}, prepared_groups{}, stepped_groups{};
    };
    static void begin_frame_measurement(bool enabled);
    [[nodiscard]] static FrameWork frame_work();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace eawr::presentation::godot_backend
