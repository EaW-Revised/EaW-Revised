#pragma once
#include "eawr/presentation/godot/renderer.hpp"
#include "eawr/presentation/shader_material_activation.hpp"
#include "eawr/vfs/vfs.hpp"
#include <iosfwd>

namespace eawr::scene { class VfsAssetCache; }

namespace eawr::presentation::godot_backend {
struct ShaderActivationReceipt final {
    shader_material::Work work;
    std::size_t snapshot_bytes{}, resident_bytes{}, effects{};
    std::uint64_t elapsed_us{};
    std::string details;
};
void write_shader_activation_report(std::ostream& output, const ShaderActivationReceipt* receipt);
// Main-thread mod-load owner. Routes receive only generation(), never this
// compiler/job owner. step() must finish before battle construction begins.
class ShaderActivationOwner final {
public:
    ~ShaderActivationOwner();
    [[nodiscard]] core::Result<void> begin(bool enabled, const vfs::Vfs& mounted,
        std::string chain_identity, std::span<const shader_activation::LayerPolicy> layers,
        godot::Node3D& host, std::shared_ptr<GodotShaderCache> shaders);
    [[nodiscard]] bool step(std::size_t budget);
    void cancel();
    [[nodiscard]] std::shared_ptr<const shader_activation::Generation> generation() const;
    [[nodiscard]] std::shared_ptr<const ShaderActivationReceipt> receipt() const { return receipt_; }
    [[nodiscard]] shader_material::Work work() const noexcept { return work_; }
    [[nodiscard]] const std::optional<shader_report::Report>& report() const noexcept { return report_; }
private:
    shader_activation::Publisher publisher_;
    std::uint64_t id_{};
    std::unique_ptr<shader_material::Compiler> compiler_;
    std::unique_ptr<shader_activation::Job> job_;
    shader_material::Work work_;
    std::shared_ptr<const ShaderActivationReceipt> receipt_;
    std::size_t snapshot_bytes_{}, effects_{};
    std::uint64_t started_us_{};
    shader_techniques::Settings selection_;
    std::optional<shader_report::Report> report_;
    std::string prewarm_refusals_;
};
// Shared loading-only adapters; no compiler is exposed to routes.
[[nodiscard]] core::Result<void> begin_mod_shaders(ShaderActivationOwner& owner, const vfs::Vfs& mounted,
    std::span<const vfs::LayerRoot> roots, std::string_view profile, godot::Node3D& host,
    std::shared_ptr<GodotShaderCache> shaders);
void install_mod_shaders(GodotRenderer& renderer, std::shared_ptr<const shader_activation::Generation> generation,
    std::shared_ptr<const vfs::Vfs> mounted);
// Unit preview owns its VFS before its renderer; that VFS must outlive renderer.
void install_mod_shaders(GodotRenderer& renderer, std::shared_ptr<const shader_activation::Generation> generation,
    const vfs::Vfs& mounted);
void admit_scene_shaders(scene::VfsAssetCache& cache, GodotRenderer& renderer);
void write_shader_route_report(std::ostream& output, const GodotRenderer* renderer);
} // namespace eawr::presentation::godot_backend
