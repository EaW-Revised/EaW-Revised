#include "eawr/assets/effect.hpp"

#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

// Caller-supplied data roots, highest precedence first. This uses the production
// mount path, including loose-only mods. No installed names or payloads are pins.
int run_effect_inventory(const int argc, const char* const* argv) {
    using namespace eawr;
    std::vector<vfs::LayerRoot> roots;
    for (int i = 2; i < argc; ++i) {
        const auto root = vfs::mod_data_root(std::filesystem::path(argv[i]));
        if (!std::filesystem::is_directory(root)) { std::cerr << "inventory data root is absent\n"; return 1; }
        roots.emplace_back("inventory-" + std::to_string(i - 2), root);
    }
    const auto resolved = vfs::resolve_manifest_chain(roots);
    if (!resolved) { std::cerr << resolved.error().code << ' ' << resolved.error().message << '\n'; return 1; }
    std::vector<vfs::MountSpec> mounts;
    for (const auto& layer : resolved.value()) mounts.push_back(layer.mount);
    auto mounted = vfs::Vfs::mount(mounts);
    if (!mounted) { std::cerr << mounted.error().code << ' ' << mounted.error().message << '\n'; return 1; }
    const auto listed = mounted.value().enumerate({}, ".fxo");
    if (!listed) { std::cerr << listed.error().code << ' ' << listed.error().message << '\n'; return 1; }
    std::size_t parsed_count{}, failure_count{}, parameters{}, techniques{}, passes{}, programs{}, constants{},
        samplers{}, expressions{}, preshaders{}, named{}, nulls{}, declarations{};
    struct LayerCounts { std::size_t effects{}, parsed{}, failures{}; };
    std::map<std::string, LayerCounts> layers;
    for (const auto& record : listed.value()) {
        auto& layer = layers[record.layer_id]; ++layer.effects;
        const auto bytes = mounted.value().open(record);
        if (!bytes) { std::cerr << bytes.error().code << ' ' << record.canonical_path << '\n'; ++failure_count; ++layer.failures; continue; }
        const auto parsed = assets::read_compiled_effect(bytes.value(), assets::source_from(record));
        if (!parsed) {
            const auto& error = parsed.error();
            std::cout << "failure\t" << record.layer_id << '\t' << record.canonical_path << '\t' << error.code << '\t'
                      << error.column.value_or(0U) << '\t' << error.message << '\n';
            ++failure_count; ++layer.failures; continue;
        }
        const auto& effect = parsed.value(); ++parsed_count; ++layer.parsed;
        parameters += effect.parameters.size(); techniques += effect.techniques.size(); programs += effect.programs.size();
        expressions += effect.expressions.size();
        for (const auto& t : effect.techniques) passes += t.passes.size();
        for (const auto& p : effect.programs) {
            constants += p.constants.size(); preshaders += p.preshaders.size(); declarations += p.declarations.size();
            for (const auto& c : p.constants) if (c.register_set == assets::EffectRegisterSet::sampler) ++samplers;
        }
        for (const auto& i : effect.initializers) if (i.parameter) ++named;
        for (const auto& state : effect.states)
            if (state.shader && state.shader->kind == assets::EffectShaderAssignmentKind::null_shader) ++nulls;
    }
    for (const auto& [name, counts] : layers)
        std::cout << "layer=" << name << " effects=" << counts.effects << " parsed=" << counts.parsed
                  << " failures=" << counts.failures << '\n';
    std::cout << "inventory effects=" << listed.value().size() << " parsed=" << parsed_count << " failures=" << failure_count
              << " parameters=" << parameters << " techniques=" << techniques << " passes=" << passes
              << " programs=" << programs << " constants=" << constants << " samplers=" << samplers
              << " expressions=" << expressions << " preshaders=" << preshaders << " named=" << named
              << " nulls=" << nulls << " declarations=" << declarations << '\n';
    return failure_count ? 1 : 0;
}
