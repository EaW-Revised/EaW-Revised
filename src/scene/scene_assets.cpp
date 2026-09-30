#include "eawr/scene/scene.hpp"

#include "scene_internal.hpp"

#include "eawr/sim/replay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace eawr::scene {

// Authored names carry the source-art suffix where the shipped asset often
// carries another, so a reference is probed as written and then by stem. This
// is the asset layer's documented rule, returning the path it found.
[[nodiscard]] std::string probe(const AssetAccess& access, const std::string_view root,
                                const std::string_view name,
                                const std::span<const std::string_view> suffixes) {
    const std::string folded = canonical(name);
    if (folded.empty() || !access.exists) return {};
    const std::string literal = std::string(root) + folded;
    if (access.exists(literal)) return literal;
    std::string stem = folded;
    for (const std::string_view suffix : suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            stem.resize(stem.size() - suffix.size());
            break;
        }
    }
    for (const std::string_view suffix : suffixes) {
        std::string candidate = std::string(root) + stem + std::string(suffix);
        if (access.exists(candidate)) return candidate;
    }
    return {};
}

[[nodiscard]] ModelFacts model_facts(const AssetAccess& access, const std::string& path) {
    ModelFacts facts;
    facts.logical_path = path;
    const assets::Model* model = access.model ? access.model(path) : nullptr;
    if (model == nullptr) {
        facts.particle_system = access.particle_system && access.particle_system(path);
        return facts;
    }
    facts.loaded = true;
    if (access.sha256) facts.sha256 = access.sha256(path);
    for (std::size_t mesh_index = 0; mesh_index < model->meshes.size(); ++mesh_index) {
        const assets::Mesh& mesh = model->meshes[mesh_index];
        // The renderer draws visible meshes only, so an invisible mesh (a
        // collision or damage-state mesh) is not a surface of the static scene.
        if (!static_mesh_visible(*model, mesh)) continue;
        for (std::size_t submesh_index = 0; submesh_index < mesh.submeshes.size(); ++submesh_index) {
            const assets::Submesh& submesh = mesh.submeshes[submesh_index];
            Surface surface;
            surface.mesh_index = static_cast<std::uint32_t>(mesh_index);
            surface.submesh_index = static_cast<std::uint32_t>(submesh_index);
            surface.mesh_name = mesh.name;
            surface.shader = submesh.shader;
            if (const LegacySelector* selector = find_legacy_selector(submesh.shader)) {
                surface.supported = true;
                surface.technique = std::string(selector->technique);
                surface.pass = std::string(selector->pass);
            }
            for (const assets::MaterialParameter& parameter : submesh.parameters) {
                if (parameter.kind != assets::ParameterKind::texture) continue;
                const auto* name = std::get_if<std::string>(&parameter.value);
                if (name == nullptr || trimmed(*name).empty()) continue;
                TextureBinding binding;
                binding.parameter = parameter.name;
                binding.declared = *name;
                binding.resolved = probe(access, "data/art/textures/", trimmed(*name), texture_suffixes);
                surface.textures.push_back(std::move(binding));
            }
            facts.surfaces.push_back(std::move(surface));
        }
    }
    for (const assets::Proxy& proxy : model->proxies) {
        AttachedEffect effect;
        effect.proxy_name = proxy.name;
        effect.bone = proxy.bone;
        effect.resolved = probe(access, "data/art/models/", proxy.name, model_suffixes);
        if (effect.resolved.empty()) {
            // Proxy names in the corpus carry an `_ALT<digits>` suffix that
            // distinguishes several proxies of one effect on one model; the
            // effect file is named without it. Only that exact trailing form
            // is removed, and only after the literal name failed.
            const std::string name = canonical(proxy.name);
            const std::size_t marker = name.rfind("_alt");
            if (marker != std::string::npos && marker + 4 < name.size()
                && std::all_of(name.begin() + static_cast<std::ptrdiff_t>(marker + 4), name.end(),
                               [](const char c) { return c >= '0' && c <= '9'; })) {
                effect.resolved = probe(access, "data/art/models/", name.substr(0, marker), model_suffixes);
                if (!effect.resolved.empty()) effect.alternate_suffix_removed = true;
            }
        }
        facts.effects.push_back(std::move(effect));
    }
    return facts;
}

bool static_mesh_visible(const assets::Model& model, const assets::Mesh& mesh) noexcept {
    if (!mesh.visible) return false;
    const auto alt = [](const std::string_view name) -> std::optional<std::string_view> {
        const std::size_t marker = name.size() >= 5 ? name.size() - 5 : 0;
        // The shipped construction models use one-digit ALT0..ALT3 suffixes.
        if (name.size() >= 5 && ieq(name.substr(marker, 4), "_ALT")
            && name.back() >= '0' && name.back() <= '9') return name.substr(marker + 4);
        return std::nullopt;
    };
    const bool has_intact_state = std::any_of(model.meshes.begin(), model.meshes.end(), [&](const assets::Mesh& item) {
        return alt(item.name) == std::optional<std::string_view>{"0"};
    });
    if (!has_intact_state) return true;
    if (mesh.name.size() >= 6 && ieq(std::string_view(mesh.name).substr(0, 6), "girder")) return false;
    const auto state = alt(mesh.name);
    return !state || *state == "0";
}

std::vector<std::uint8_t> capture_variant_bones(
    const assets::Model& model, const std::span<const assets::Animation> idle_clips) {
    std::vector<std::uint8_t> seen_hidden(model.bones.size());
    std::vector<std::uint8_t> seen_visible(model.bones.size());
    for (const assets::Animation& clip : idle_clips) {
        std::vector<std::uint8_t> visible;
        visible.reserve(model.bones.size());
        for (const assets::Bone& bone : model.bones) visible.push_back(bone.visible ? 1U : 0U);
        for (const assets::AnimationTrack& track : clip.tracks) {
            if (track.bone_index >= model.bones.size() || track.samples.empty()
                || !ieq(track.bone_name, model.bones[track.bone_index].name)) continue;
            visible[track.bone_index] = track.samples.front().visible ? 1U : 0U;
        }
        for (std::size_t index = 0; index < visible.size(); ++index) {
            if (visible[index]) seen_visible[index] = 1U;
            else seen_hidden[index] = 1U;
        }
    }
    std::vector<std::uint8_t> variant(model.bones.size());
    for (std::size_t index = 0; index < variant.size(); ++index) {
        variant[index] = seen_hidden[index] && seen_visible[index] ? 1U : 0U;
    }
    return variant;
}

bool uncaptured_mesh_visible(const assets::Model& model, const assets::Mesh& mesh,
                             const std::span<const std::uint8_t> variant_bones) noexcept {
    if (!static_mesh_visible(model, mesh)) return false;
    for (std::int32_t bone = mesh.bone; bone >= 0;) {
        const auto index = static_cast<std::size_t>(bone);
        if (index >= model.bones.size() || !model.bones[index].visible
            || (index < variant_bones.size() && variant_bones[index] != 0U)) return false;
        const std::int32_t parent = model.bones[index].parent;
        if (parent == bone) return false;
        bone = parent;
    }
    return true;
}

VfsAssetCache::VfsAssetCache(const vfs::Vfs& filesystem) : filesystem_(&filesystem) {}

AssetAccess VfsAssetCache::access() {
    return AssetAccess{
        [this](const std::string_view path) { return exists(path); },
        [this](const std::string_view path) { return model(path); },
        [this](const std::string_view path) { return sha256(path); },
        [this](const std::string_view path) { return particle_system(path); },
    };
}

bool VfsAssetCache::exists(const std::string_view logical_path) {
    const auto found = exists_.find(logical_path);
    if (found != exists_.end()) return found->second;
    const bool present = static_cast<bool>(filesystem_->stat(logical_path));
    exists_.emplace(std::string(logical_path), present);
    return present;
}

const assets::Model* VfsAssetCache::model(const std::string_view logical_path) {
    auto found = models_.find(logical_path);
    if (found == models_.end()) {
        Loaded loaded;
        auto decoded = assets::load_model(*filesystem_, logical_path);
        if (decoded) {
            loaded.model = std::move(decoded.value());
        } else {
            // The model loader rejects a particle-system root explicitly with
            // `unsupported`; that is a distinct cause, not a decode failure.
            loaded.particle_system = decoded.error().code == assets::diagnostic_codes::unsupported
                && decoded.error().message.find("particle-system") != std::string::npos;
        }
        found = models_.emplace(std::string(logical_path), std::move(loaded)).first;
    }
    return found->second.model ? &*found->second.model : nullptr;
}

std::string VfsAssetCache::sha256(const std::string_view logical_path) {
    const auto found = hashes_.find(logical_path);
    if (found != hashes_.end()) return found->second;
    std::string hash;
    if (auto bytes = filesystem_->open(logical_path)) {
        hash = sim::sha256_hex(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.value().data()), bytes.value().size()));
    }
    hashes_.emplace(std::string(logical_path), hash);
    return hash;
}

bool VfsAssetCache::particle_system(const std::string_view logical_path) {
    static_cast<void>(model(logical_path));
    const auto found = models_.find(logical_path);
    return found != models_.end() && found->second.particle_system;
}

} // namespace eawr::scene
