// Manual generator: compile/link with the documented BASE production sources,
// never with the optimized helpers. See fixtures/setup-map-golden.md.
#include "setup_map_golden.hpp"
#include <iostream>
using namespace eawr;
static vfs::Vfs mount(const std::vector<vfs::LayerRoot>& roots) {
    auto chain=vfs::resolve_manifest_chain(roots);if(!chain)throw std::runtime_error(core::format_diagnostic(chain.error()));
    std::vector<vfs::MountSpec> specs;for(auto& part:chain.value())specs.push_back(std::move(part.mount));
    auto fs=vfs::Vfs::mount(specs);if(!fs)throw std::runtime_error(core::format_diagnostic(fs.error()));return std::move(fs).value();
}
static void capture(const std::filesystem::path& target,const vfs::Vfs& fs,const data::Catalog& catalog) {
    auto maps=skirmish::setup_maps(fs,catalog);if(!maps)throw std::runtime_error(core::format_diagnostic(maps.error()));
    std::ofstream out(target);
    const auto list_hash=setup_map_golden::map_digest(maps.value());
    out<<"@maps "<<list_hash<<' '<<list_hash<<' '<<list_hash<<'\n';
    for(const auto& map:maps.value()) {
        const auto row=setup_map_golden::row(map,fs,catalog);
        out<<map.path<<' '<<row.map<<' '<<row.fixture<<' '<<row.inputs<<'\n';
    }
    if(!out)throw std::runtime_error("Could not write golden hashes");
    std::cout<<target.filename().string()<<" maps "<<maps.value().size()<<std::endl;
}
int main(int argc,char** argv) {
    if(argc!=4) { std::cerr<<"capture_setup_map_golden <game root> <synthetic directory> <output directory>\n";return 2; }
    try {
        const std::filesystem::path game(argv[1]),synthetic(argv[2]),out(argv[3]);std::filesystem::create_directories(out);
        auto fs=mount({{"expansion",game/"corruption/Data"},{"base",game/"GameData/Data"}});
        auto catalog=data::load_catalog_strict(fs,data::Profile::foc);if(!catalog)throw std::runtime_error(core::format_diagnostic(catalog.error()));
        capture(out/"setup-map-eager-foc.sha256",fs,catalog.value().catalog);
        setup_map_golden::write_team_corpus(synthetic);
        const vfs::MountSpec spec{.layer_id="synthetic-team",.data_root=synthetic,.loose_logical_prefix="data",.active_archives={}};
        auto teamfs=vfs::Vfs::mount(std::span(&spec,1));if(!teamfs)throw std::runtime_error(core::format_diagnostic(teamfs.error()));
        const data::Catalog empty;capture(out/"setup-map-eager-empty-catalog.sha256",teamfs.value(),empty);
        auto teamcatalog=data::load_catalog(teamfs.value(),data::Profile::foc);if(!teamcatalog)throw std::runtime_error(core::format_diagnostic(teamcatalog.error()));
        capture(out/"setup-map-eager-team.sha256",teamfs.value(),teamcatalog.value().catalog);
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    return 0;
}
