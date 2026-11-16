#include "texture_volumes.hpp"

#include "viewer_path.hpp"
#include "eawr/units/simulation_model.hpp"

#include <filesystem>
#include <system_error>
#include <vector>

namespace eawr::presentation::godot_backend {

bool game_data_volumes_solid_state(const std::string& game_root, const std::string& mod_root) {
    std::vector<std::filesystem::path> roots{ViewerPath{game_root}.native()};
    if (!mod_root.empty()) roots.push_back(ViewerPath{mod_root}.native());
    std::vector<units::DiskKind> kinds;
    for (const auto& root : roots) {
        std::error_code error;
        const auto absolute = std::filesystem::absolute(root, error);
        kinds.push_back(error ? units::DiskKind::unknown : units::volume_disk_kind(absolute.root_name().wstring()));
    }
    return units::slowest_disk(kinds) == units::DiskKind::solid_state;
}

} // namespace eawr::presentation::godot_backend
