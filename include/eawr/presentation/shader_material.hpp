#pragma once

#include "eawr/presentation/renderer.hpp"
#include "eawr/presentation/shader_evaluation.hpp"
#include "eawr/presentation/shader_lowering.hpp"
#include "eawr/presentation/shader_stage.hpp"
#include "eawr/presentation/shader_techniques.hpp"

namespace eawr::presentation::shader_material {
inline constexpr std::uint32_t policy_version = 8U;
namespace diagnostic_codes {
inline constexpr std::string_view interface_mismatch = "EAWR-SHADER-MATERIAL-0001";
inline constexpr std::string_view semantic = "EAWR-SHADER-MATERIAL-0002";
inline constexpr std::string_view resource = "EAWR-SHADER-MATERIAL-0003";
inline constexpr std::string_view state = "EAWR-SHADER-MATERIAL-0004";
inline constexpr std::string_view limit = "EAWR-SHADER-MATERIAL-0005";
}
struct ClipDepthTransform final { std::int32_t z_scale{}, w_scale{}; };
// The selected bridge preserves D3D [0,w] depth. Both target backends
// reverse it: Compatibility [-w,w], RenderingDevice [0,w].
[[nodiscard]] inline constexpr ClipDepthTransform clip_depth_transform(bool compatibility) noexcept {
    return {compatibility ? -2 : -1, 1};
}
struct Uniform final {
    std::string name;
    shader_lowering::Type type;
    std::size_t word{};
    std::size_t binding{};
    bool instance{};
};
struct Stage final {
    shader_evaluation::Plan updates;
    std::vector<Uniform> uniforms;
};
struct Texture final {
    std::string uniform, parameter, default_name;
    std::optional<shader_bindings::EngineInput> engine;
};
struct ParameterRecipe final {
    std::size_t parameter{};
    std::string name;
    shader_bindings::NumericShape shape;
};
// Owned CPU program. Retains neither borrowed effect bytes nor compiler handles.
struct Program final {
    MaterialDescription material;
    // Actually read authored vectors/colour/secondary UV require a renderer
    // payload whose precision and skin transforms are qualified separately.
    bool extended_vertex_inputs{};
    Stage vertex, fragment;
    std::vector<ParameterRecipe> parameters;
    std::vector<Texture> textures;
    std::vector<MaterialBinding> instance_defaults;
    std::size_t retained_bytes{};
};
struct Limits final {
    std::size_t source_bytes{1024U * 1024U}, ports{256U}, uniforms{4096U};
};
// S-13..21: reconcile actual backend reflection with the lowerer's ports and
// the typed binding/update plans. Unknown vertex associations, resources,
// states and sampler realizations refuse; emitted identifier spelling is
// never used as a D3D semantic or register identity.
[[nodiscard]] core::Result<Program> assemble(const assets::EffectView& effect,
    const shader_runtime::StageArtifact& vertex, const shader_runtime::StageArtifact& fragment,
    const shader_techniques::PassBoundary& pass, Limits limits = {});

struct Instance final {
    MaterialDescription material;
    shader_evaluation::Output vertex, fragment;
    std::vector<MaterialBinding> parameters;
    bool valid{};
};
// Allocate outside draw; update retains all buffers and binding names.
[[nodiscard]] Instance make_instance(const Program& program);
[[nodiscard]] core::Result<void> update(const Program& program,
    shader_evaluation::Inputs inputs, Instance& instance);
} // namespace eawr::presentation::shader_material
