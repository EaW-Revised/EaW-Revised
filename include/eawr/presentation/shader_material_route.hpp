#pragma once
#include "eawr/presentation/shader_material.hpp"

namespace eawr::presentation::shader_activation { struct Generation; }
#include <array>
#include <memory>
#include <iosfwd>

namespace eawr::presentation::shader_material::routing {
// Unknown driver names cannot stand in for actual PCI vendor identity.
[[nodiscard]] std::optional<std::uint32_t> recognized_pci_vendor(std::string_view name) noexcept;
using Matrix = std::array<float, 16U>;
// Column-major coefficients. The row-vector source sees their transpose;
// P5 alone applies the declared CTAB register packing.
[[nodiscard]] Matrix multiply(const Matrix& left, const Matrix& right) noexcept;
// D3D left-handed [0,w] depth, before the wrapper's target-depth conversion.
[[nodiscard]] std::optional<Matrix> source_projection(const FixedCamera& camera) noexcept;
void write_route_report(std::ostream& output, bool enabled, std::size_t uploaded,
    std::size_t approximated, std::size_t refused, std::size_t missing_green);
enum class ProviderRefusal { none, fog_variant, colorization, vertex_attributes, skinning };
[[nodiscard]] ProviderRefusal provider_refusal(const Program& program, bool fog_enabled, bool palette_skinned = false) noexcept;
struct TextureRequest final { std::string uniform, resource; };
// Providers return an existing resolved resource identity, never a replacement
// for an absent image. They run during surface preparation, outside draw.
// Borrowed for one bind() call. Store the callable itself when reusing it;
// this view neither allocates nor extends a lambda's lifetime.
template<class Input> class ResourceLookup final {
public:
    ResourceLookup() = default;
    template<class Callable> ResourceLookup(const Callable& callable)
        : context_(std::addressof(callable)), invoke_([](const void* context, Input value) -> std::optional<std::string> {
            return (*static_cast<const Callable*>(context))(value);
        }) {}
    [[nodiscard]] explicit operator bool() const noexcept { return invoke_ != nullptr; }
    [[nodiscard]] std::optional<std::string> operator()(Input value) const {
        return invoke_ ? invoke_(context_, value) : std::nullopt;
    }
private:
    const void* context_{};
    std::optional<std::string> (*invoke_)(const void*, Input){};
};
using ResolveTexture = ResourceLookup<std::string_view>;
using EngineTexture = ResourceLookup<shader_bindings::EngineInput>;
struct NumericOverride final {
    std::size_t parameter{}, count{};
    assets::EffectValueKind kind{};
    std::array<std::uint32_t, 4U> words{};
};
struct Surface final {
    std::shared_ptr<const shader_activation::Generation> generation;
    const Program* program{};
    Instance instance;
    std::vector<TextureRequest> textures;
    std::vector<NumericOverride> overrides;
    std::vector<shader_evaluation::ParameterInput> inputs;
    Surface(std::shared_ptr<const shader_activation::Generation> owner, const Program& value)
        : generation(std::move(owner)), program(&value), instance(make_instance(value)) {}
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    Surface(Surface&&) noexcept = default;
    Surface& operator=(Surface&&) noexcept = default;
    // Reuses all numeric buffers. Missing current providers invalidate the
    // complete surface; callers must select the explicit fallback.
    [[nodiscard]] core::Result<void> update(std::span<const shader_evaluation::EngineInput> current);
};
// Same canonical identity as the activation inventory, including subdirectories.
[[nodiscard]] core::Result<std::string> effect_identity(std::string_view shader);
// Null generation or absent admitted effect returns an empty success: the
// existing route applies. A present program with unusable bindings returns a
// named failure. This API has no compiler or cache and cannot translate late.
[[nodiscard]] core::Result<std::unique_ptr<Surface>> bind(
    std::shared_ptr<const shader_activation::Generation> generation, std::string_view shader,
    std::span<const assets::MaterialParameter> parameters,
    std::span<const shader_evaluation::EngineInput> current,
    const ResolveTexture& resolve, const EngineTexture& engine = {},
    std::span<const MaterialBinding> overrides = {});
} // namespace eawr::presentation::shader_material::routing
