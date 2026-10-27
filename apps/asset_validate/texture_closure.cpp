#include "asset_validate_internal.hpp"
#include "eawr/core/json_text.hpp"
#include "eawr/core/text.hpp"
#include "eawr/presentation/particles/particles.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/skirmish/setup.hpp"
// Diagnostic tool: use exactly the scene loader's current candidate sequence.
#include "../../src/scene/scene_internal.hpp"

#include <set>

namespace asset_validate_internal {
namespace {
std::string json_string(std::string_view value) { return eawr::core::json_quote_ascii(value); }
std::string filename(std::string value) {
    std::replace(value.begin(), value.end(), '\\', '/');
    const auto slash = value.find_last_of('/');
    return eawr::core::to_lower(slash == std::string::npos ? value : value.substr(slash + 1));
}
std::uint32_t word(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) return 0;
    std::uint32_t value{};
    for (unsigned i = 0; i < 4; ++i) value |= std::to_integer<std::uint32_t>(bytes[offset + i]) << (8U * i);
    return value;
}
std::string header(std::span<const std::byte> bytes) {
    if (word(bytes, 0) == 0x20534444U) {
        const auto caps = word(bytes, 112);
        std::string shape = (caps & 0x200000U) ? "volume" : (caps & 0x200U) ? "cube" : "2d";
        const auto code = word(bytes, 84);
        if (code == 0x30315844U) return "DDS/DXGI-" + std::to_string(word(bytes, 128)) + "/" + shape;
        if (code != 0) {
            std::string fourcc;
            for (unsigned i = 0; i < 4; ++i) {
                const auto c = static_cast<char>((code >> (8U * i)) & 255U);
                fourcc += c >= 32 && c <= 126 ? c : '?';
            }
            return "DDS/" + fourcc + "/" + shape;
        }
        return "DDS/RGB-" + std::to_string(word(bytes, 88)) + "/" + shape;
    }
    if (bytes.size() >= 8 && word(bytes, 0) == 0x474e5089U && word(bytes, 4) == 0x0a1a0a0dU) return "PNG";
    if (bytes.size() >= 3 && bytes[0] == std::byte{255} && bytes[1] == std::byte{216}
        && bytes[2] == std::byte{255}) return "JPG";
    if (bytes.size() >= 2 && bytes[0] == std::byte{'B'} && bytes[1] == std::byte{'M'}) return "BMP";
    if (bytes.size() >= 18) {
        const auto type = std::to_integer<unsigned>(bytes[2]);
        const auto depth = std::to_integer<unsigned>(bytes[16]);
        if ((type == 2 || type == 3 || type == 10 || type == 11) && (depth == 8 || depth == 24 || depth == 32)) return "TGA";
    }
    return "other";
}
struct Scan {
    const eawr::vfs::Vfs& fs;
    std::unique_ptr<eawr::scene::VfsAssetCache> cache;
    std::set<std::string> models, pending, references, shaders;
    std::map<std::string, std::string> model_failures;
    explicit Scan(const eawr::vfs::Vfs& filesystem) : fs(filesystem) { clear_cache(); }
    void clear_cache() { cache = std::make_unique<eawr::scene::VfsAssetCache>(fs); }
    void reference(std::string name) {
        name = filename(std::move(name));
        if (!name.empty()) references.insert(std::move(name));
    }
    void model(std::string name) {
        name = filename(std::move(name));
        if (name.empty()) return;
        const auto access = cache->access();
        const std::array<std::string_view, 1> suffixes{".alo"};
        const auto path = eawr::scene::probe(access, "data/art/models/", name, suffixes);
        if (!path.empty() && !models.contains(path)) pending.insert(path);
    }
    void drain() {
        while (!pending.empty()) {
            const auto path = *pending.begin(); pending.erase(pending.begin());
            if (!models.insert(path).second) continue;
            const auto* mesh = cache->model(path);
            if (mesh != nullptr) {
                for (const auto& part : mesh->meshes)
                    for (const auto& surface : part.submeshes) shaders.insert(filename(surface.shader));
                for (const auto& surface : eawr::scene::construction_surfaces(cache->access(), path))
                    for (const auto& binding : surface.textures) reference(binding.declared);
                for (const auto& effect : eawr::scene::model_proxy_effects(cache->access(), *mesh)) model(effect.resolved);
                for (const auto& dazzle : mesh->dazzles) reference(dazzle.texture);
            } else {
                auto bytes = fs.open(path);
                if (!bytes) { model_failures[path] = bytes.error().message; continue; }
                auto particles = eawr::presentation::particles::load_alo(bytes.value(), path);
                if (!particles) { model_failures[path] = particles.error().message; continue; }
                for (const auto& emitter : particles.value().emitters) {
                    reference(emitter.color_texture); reference(emitter.normal_texture);
                }
            }
            clear_cache();
        }
    }
    void write(std::ostream& out) {
        drain();
        std::map<std::string, std::size_t> formats, rejects;
        std::set<std::string> decoded_paths;
        std::size_t missing{}, aliases{}, rejected{};
        out << "{\"model_count\":" << models.size() << ",\"references\":[";
        bool first = true;
        for (const auto& name : references) {
            if (!first) out << ',';
            first = false;
            const std::array<std::string_view, 2> suffixes{".tga", ".dds"};
            const auto path = eawr::scene::probe(cache->access(), "data/art/textures/", name, suffixes);
            out << "{\"name\":" << json_string(name) << ",\"resolved\":" << json_string(path);
            if (path.empty()) {
                ++missing;
                const auto dot = name.find_last_of('.');
                const auto alias = "data/art/textures/" + name.substr(0, dot) + ".dds";
                const bool exists = bool(fs.stat(alias));
                aliases += exists ? 1U : 0U;
                out << ",\"dds_alias\":" << (exists ? json_string(alias) : "null");
            } else {
                auto bytes = fs.open(path);
                auto record = fs.stat(path);
                if (bytes && record) {
                    const auto format = header(bytes.value());
                    out << ",\"header\":" << json_string(format) << ",\"layer\":" << json_string(record.value().layer_id);
                    auto texture = eawr::assets::load_texture(bytes.value(), eawr::assets::source_from(record.value()));
                    out << ",\"accepted\":" << (texture ? "true" : "false");
                    if (!texture) out << ",\"reason\":" << json_string(texture.error().message);
                    if (decoded_paths.insert(path).second) {
                        ++formats[format];
                        if (!texture) { ++rejected; ++rejects[texture.error().message]; }
                    }
                } else out << ",\"read_error\":true";
            }
            out << '}';
        }
        out << "],\"unique_names\":" << references.size() << ",\"unique_resolved_files\":" << decoded_paths.size()
            << ",\"missing_names\":" << missing << ",\"missing_with_dds_alias\":" << aliases
            << ",\"rejected_files\":" << rejected << ",\"formats\":{";
        first = true;
        for (const auto& [key, count] : formats) { if (!first) out << ','; first = false; out << json_string(key) << ':' << count; }
        out << "},\"reject_reasons\":{";
        first = true;
        for (const auto& [key, count] : rejects) { if (!first) out << ','; first = false; out << json_string(key) << ':' << count; }
        out << "},\"model_failures\":{";
        first = true;
        for (const auto& [key, reason] : model_failures) { if (!first) out << ','; first = false; out << json_string(key) << ':' << json_string(reason); }
        out << "},\"shader_references\":[";
        std::set<std::string> shader_files;
        first = true;
        for (const auto& shader : shaders) {
            if (shader.empty()) continue;
            if (!first) out << ',';
            first = false;
            out << json_string(shader);
            const auto dot = shader.find_last_of('.');
            for (const auto suffix : {".fx", ".fxo"}) {
                const auto path = "data/art/shaders/" + shader.substr(0, dot) + suffix;
                if (fs.stat(path)) shader_files.insert(path);
            }
        }
        out << "],\"shader_files\":[";
        std::map<std::string, std::size_t> shader_containers, shader_profiles;
        first = true;
        for (const auto& path : shader_files) {
            auto bytes = fs.open(path);
            if (!bytes) continue;
            if (!first) out << ',';
            first = false;
            const auto suffix = path.substr(path.find_last_of('.'));
            ++shader_containers[suffix];
            std::set<std::string> profiles;
            // Shader version tokens are aligned DWORDs in the compiled effect.
            // Count each observed profile at most once per referenced file.
            if (suffix == ".fxo") for (std::size_t offset = 0; offset + 4 <= bytes.value().size(); offset += 4) {
                const auto token = word(bytes.value(), offset);
                const auto kind = token >> 16U;
                const auto major = (token >> 8U) & 255U;
                const auto minor = token & 255U;
                if ((kind == 0xfffeU || kind == 0xffffU) && major >= 1 && major <= 3 && minor <= 1)
                    profiles.insert(std::string(kind == 0xfffeU ? "vs_" : "ps_") + std::to_string(major) + '_' + std::to_string(minor));
            }
            out << "{\"path\":" << json_string(path) << ",\"first_word\":" << word(bytes.value(), 0) << ",\"profiles\":[";
            bool profile_first = true;
            for (const auto& profile : profiles) {
                if (!profile_first) out << ',';
                profile_first = false;
                out << json_string(profile); ++shader_profiles[profile];
            }
            out << "]}";
        }
        out << "],\"shader_containers\":{";
        first = true;
        for (const auto& [key, count] : shader_containers) { if (!first) out << ','; first = false; out << json_string(key) << ':' << count; }
        out << "},\"shader_profiles_observed\":{";
        first = true;
        for (const auto& [key, count] : shader_profiles) { if (!first) out << ','; first = false; out << json_string(key) << ':' << count; }
        out << "}}";
    }
};
} // namespace

int texture_closure(const eawr::vfs::Vfs& fs, const Arguments& args) {
    auto loaded = eawr::data::load_catalog(fs, eawr::data::Profile::remake);
    if (!loaded) { std::cerr << loaded.error().message << '\n'; return 1; }
    const auto& catalog = loaded.value().catalog;
    Scan maps(fs), objects(fs);
    std::size_t space{}, land{}, failed_maps{};
    auto records = fs.enumerate("data/art/maps/", ".ted");
    if (!records) return 1;
    const auto types = eawr::assets::object_type_catalog(catalog);
    for (const auto& record : records.value()) {
        auto map = eawr::assets::load_map(fs, record.canonical_path, types);
        if (!map) { ++failed_maps; continue; }
        eawr::skirmish::SetupMapQuery query;
        query.kind = map.value().kind.value_or(eawr::assets::MapKind::space);
        if (!eawr::skirmish::setup_map_eligible(map.value(), query)) continue;
        if (query.kind == eawr::assets::MapKind::space) ++space; else ++land;
        const auto resolution = eawr::assets::resolve_map(map.value(), types,
            [&fs](std::string_view path) { return bool(fs.stat(path)); });
        for (const auto& environment : resolution.environment_references)
            if (environment.model_name) maps.model(*environment.model_name);
        auto scene = eawr::scene::build({&map.value(), {}, &catalog, maps.cache->access()});
        for (const auto& asset : scene.assets) maps.model(asset.logical_path);
        for (const auto& placement : scene.placements)
            for (const auto& surface : placement.surfaces)
                for (const auto& binding : surface.textures) maps.reference(binding.declared);
        if (map.value().terrain) for (const auto& material : map.value().terrain->materials) {
            if (material.primary_texture) maps.reference(*material.primary_texture);
            if (material.secondary_texture) maps.reference(*material.secondary_texture);
        }
        for (const auto& texture : map.value().water_textures) maps.reference(texture.logical_name);
        for (const auto& environment : map.value().environments)
            if (environment.cloud_texture) maps.reference(*environment.cloud_texture);
        maps.clear_cache();
    }
    // Deliberately separate this conservative superset from placed skirmish art:
    // it also contains campaign-only active objects, never a raw asset-file walk.
    for (const auto& definition : catalog.definitions()) {
        if (!definition.active || !definition.namespace_winner || definition.category != eawr::data::Category::game_object) continue;
        auto object = catalog.resolve(definition.id, definition.category);
        if (!object) continue;
        for (const auto& value : object.value().values) {
            if (eawr::core::to_lower(value.value.name).find("model") == std::string::npos) continue;
            std::istringstream names(value.value.raw_text);
            std::string name;
            while (names >> name) { if (name.back() == ',') name.pop_back(); objects.model(name); }
        }
    }
    std::ofstream out(args.report);
    if (!out) return 1;
    out << "{\"space_maps\":" << space << ",\"land_maps\":" << land << ",\"failed_maps\":" << failed_maps
        << ",\"catalog_health\":" << json_string(eawr::data::describe_load_health(loaded.value().health))
        << ",\"skirmish_placed_art\":";
    maps.write(out);
    out << ",\"active_object_model_superset\":";
    objects.write(out);
    out << "}\n";
    return out ? 0 : 1;
}
} // namespace asset_validate_internal
