#include "eawr/presentation/shader_material_route.hpp"
#include "eawr/presentation/shader_material_activation.hpp"
#include "eawr/vfs/vfs.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <ostream>
#include <type_traits>

namespace eawr::presentation::shader_material::routing {
namespace {
core::Diagnostic problem(std::string_view message) {
    return {.code = "EAWR-SHADER-ROUTE-0001", .message = std::string(message),
        .logical_path = {}, .line = {}, .column = {}, .source_id = {}};
}
const assets::MaterialParameter* parameter(std::span<const assets::MaterialParameter> values,
    std::string_view name, bool& duplicate) {
    const assets::MaterialParameter* found{};
    for (const auto& value : values) if (value.name == name) {
        if (found) duplicate = true;
        found = &value;
    }
    return found;
}
bool numeric(const assets::MaterialParameter& source, const ParameterRecipe& recipe, NumericOverride& out) {
    if (static_cast<std::size_t>(source.kind) != source.value.index()) return false;
    out.parameter = recipe.parameter;
    bool valid = true;
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) valid = false;
        else if constexpr (std::is_same_v<T, std::int32_t>) {
            out.kind = assets::EffectValueKind::integer; out.count = 1U;
            out.words[0U] = std::bit_cast<std::uint32_t>(value);
        } else {
            out.kind = assets::EffectValueKind::floating;
            const auto word = [&](float item) {
                if (!std::isfinite(item)) valid = false;
                return std::bit_cast<std::uint32_t>(item);
            };
            if constexpr (std::is_same_v<T, float>) { out.count = 1U; out.words[0U] = word(value); }
            else {
                out.count = std::is_same_v<T, assets::Vec3f> ? 3U : 4U;
                out.words[0U] = word(value.x); out.words[1U] = word(value.y); out.words[2U] = word(value.z);
                if constexpr (std::is_same_v<T, assets::Vec4f>) out.words[3U] = word(value.w);
            }
        }
    }, source.value);
    return valid && out.kind == recipe.shape.kind
        && std::uint64_t{recipe.shape.rows} * recipe.shape.columns * recipe.shape.elements == out.count;
}
} // namespace
std::optional<std::uint32_t> recognized_pci_vendor(std::string_view name) noexcept {
    constexpr std::array<std::pair<std::string_view, std::uint32_t>, 10U> vendors{{
        {"AMD", 0x1002U}, {"ImgTec", 0x1010U}, {"Apple", 0x106bU}, {"NVIDIA", 0x10deU},
        {"ARM", 0x13b5U}, {"Microsoft", 0x1414U}, {"Qualcomm", 0x5143U}, {"Intel", 0x8086U},
        {"ATI Technologies Inc.", 0x1002U}, {"NVIDIA Corporation", 0x10deU}}};
    for (const auto& [vendor, id] : vendors) if (name == vendor) return id;
    return {};
}
ProviderRefusal provider_refusal(const Program& program, bool fog_enabled, bool palette_skinned) noexcept {
    if (fog_enabled) return ProviderRefusal::fog_variant;
    if (palette_skinned) return ProviderRefusal::skinning;
    if (program.extended_vertex_inputs) return ProviderRefusal::vertex_attributes;
    if (std::any_of(program.parameters.begin(), program.parameters.end(),
        [](const auto& value) { return value.name == "Colorization"; })) return ProviderRefusal::colorization;
    return ProviderRefusal::none;
}
void write_route_report(std::ostream& output, bool enabled, std::size_t uploaded,
    std::size_t approximated, std::size_t refused, std::size_t missing_green) {
    if (!enabled) return;
    output << "  \"shader_translation\":{\"selection_policy\":\"desktop-ST-O1\",\"uploaded\":" << uploaded
        << ",\"approximated\":" << approximated << ",\"refused\":" << refused
        << ",\"missing_green\":" << missing_green << "},\n";
}
Matrix multiply(const Matrix& left, const Matrix& right) noexcept {
    Matrix result{};
    for (std::size_t column = 0U; column < 4U; ++column) for (std::size_t row = 0U; row < 4U; ++row)
        for (std::size_t item = 0U; item < 4U; ++item) result[column * 4U + row] += left[item * 4U + row] * right[column * 4U + item];
    return result;
}
std::optional<Matrix> source_projection(const FixedCamera& camera) noexcept {
    if (!camera.width || !camera.height || !(camera.near_plane > 0.0F) || !(camera.far_plane > camera.near_plane)
        || !std::isfinite(camera.far_plane) || !(camera.vertical_fov_degrees > 0.0F && camera.vertical_fov_degrees < 180.0F)) return {};
    const auto focal = 1.0F / std::tan(camera.vertical_fov_degrees * 0.008726646259971648F);
    Matrix projection{};
    projection[0U] = focal * static_cast<float>(camera.height) / static_cast<float>(camera.width);
    projection[5U] = focal; projection[10U] = camera.far_plane / (camera.far_plane - camera.near_plane);
    projection[11U] = 1.0F; projection[14U] = -camera.near_plane * projection[10U];
    if (std::any_of(projection.begin(), projection.end(), [](float value) { return !std::isfinite(value); })) return {};
    return projection;
}
core::Result<std::string> effect_identity(std::string_view shader) {
    if (shader.empty() || shader.size() > 4096U)
        return core::Result<std::string>::failure(problem("effect identity is empty or exceeds its budget"));
    auto canonical = vfs::canonicalize(shader);
    if (!canonical) return core::Result<std::string>::failure(canonical.error());
    auto name = std::move(canonical).value();
    if (!name.starts_with("data/")) name.insert(0U, "data/art/shaders/");
    if (name.ends_with(".fxo")) name.resize(name.size() - 4U);
    else if (name.ends_with(".fx")) name.resize(name.size() - 3U);
    if (name.size() > 4096U || name == "data/art/shaders/")
        return core::Result<std::string>::failure(problem("canonical effect identity is empty or exceeds its budget"));
    return core::Result<std::string>::success(std::move(name));
}
core::Result<void> Surface::update(std::span<const shader_evaluation::EngineInput> current) {
    return shader_material::update(*program, {inputs, current}, instance);
}
core::Result<std::unique_ptr<Surface>> bind(std::shared_ptr<const shader_activation::Generation> generation,
    std::string_view shader, std::span<const assets::MaterialParameter> parameters,
    std::span<const shader_evaluation::EngineInput> current, const ResolveTexture& resolve, const EngineTexture& engine,
    std::span<const MaterialBinding> overrides) {
    using Result = core::Result<std::unique_ptr<Surface>>;
    if (!generation) return Result::success({});
    const auto identity = effect_identity(shader);
    if (!identity) return Result::failure(identity.error());
    const auto* resident = find(*generation, identity.value());
    if (!resident) return Result::success({});
    if (parameters.size() > 4096U || overrides.size() > 4096U) return Result::failure(problem("surface parameter budget exceeded"));
    for (const auto& value : parameters) {
        const auto* text = std::get_if<std::string>(&value.value);
        if (value.name.size() > 4096U || (text && text->size() > 4096U))
            return Result::failure(problem("surface parameter identifier budget exceeded"));
    }
    auto surface = std::make_unique<Surface>(std::move(generation), resident->program);
    surface->overrides.reserve(resident->program.parameters.size());
    for (const auto& recipe : resident->program.parameters) {
        bool duplicate{};
        const auto* value = parameter(parameters, recipe.name, duplicate);
        if (duplicate) return Result::failure(problem("surface has duplicate used numeric parameters"));
        const MaterialBinding* replacement{};
        for (const auto& binding : overrides) if (binding.name == recipe.name) {
            if (replacement) return Result::failure(problem("surface has duplicate used runtime overrides"));
            replacement = &binding;
        }
        std::optional<assets::MaterialParameter> runtime;
        if (replacement) {
            runtime = assets::MaterialParameter{replacement->name,
                static_cast<assets::ParameterKind>(replacement->value.index()), replacement->value};
            value = &*runtime;
        }
        if (!value) continue;
        NumericOverride override_value;
        if (!numeric(*value, recipe, override_value)) return Result::failure(problem("surface numeric parameter has an incompatible type or shape"));
        surface->overrides.push_back(override_value);
    }
    // Build borrows only after storage reaches its final size. Moving Surface
    // transfers vector buffers; copying it is forbidden.
    surface->inputs.reserve(surface->overrides.size());
    for (const auto& value : surface->overrides)
        surface->inputs.push_back({value.parameter, {value.kind, {value.words.data(), value.count}}});
    // Refuse missing numeric providers before decoding any effect images.
    if (auto updated = surface->update(current); !updated) return Result::failure(updated.error());
    for (const auto& texture : resident->program.textures) {
        std::string declared = texture.default_name;
        if (texture.engine) {
            const auto resource = engine ? engine(*texture.engine) : std::nullopt;
            if (!resource || resource->empty()) return Result::failure(problem("surface engine texture provider is unavailable"));
            declared = *resource;
        } else if (!texture.parameter.empty()) {
            bool duplicate{};
            if (const auto* value = parameter(parameters, texture.parameter, duplicate)) {
                const auto* name = std::get_if<std::string>(&value->value);
                if (!name || value->kind != assets::ParameterKind::texture)
                    return Result::failure(problem("surface texture parameter has an incompatible type"));
                declared = *name;
            }
            if (duplicate) return Result::failure(problem("surface has duplicate used texture parameters"));
        }
        const auto resource = resolve && !declared.empty() ? resolve(declared) : std::nullopt;
        if (!resource || resource->empty()) return Result::failure(problem("surface texture resource is unavailable"));
        surface->textures.push_back({texture.uniform, *resource});
        const auto binding = std::find_if(surface->instance.material.bindings.begin(), surface->instance.material.bindings.end(),
            [&](const auto& value) { return value.name == texture.uniform; });
        if (binding == surface->instance.material.bindings.end()) return Result::failure(problem("surface sampler binding is absent"));
        binding->value = *resource;
    }
    return Result::success(std::move(surface));
}
} // namespace eawr::presentation::shader_material::routing
