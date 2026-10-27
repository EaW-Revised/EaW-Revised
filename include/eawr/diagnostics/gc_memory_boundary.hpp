#pragma once

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <string_view>

namespace eawr::diagnostics {
inline bool gc_memory_boundary_enabled(bool attribution, bool interactive) {
    return attribution && !interactive;
}

template<class Capture>
void with_gc_memory_boundary(bool attribution, bool interactive, std::string_view phase, Capture&& capture) {
    if (gc_memory_boundary_enabled(attribution, interactive) && (phase == "post_load" || phase == "steady")) capture();
}

// The snapshot is an open object; attribution closes its final field below.
template<class Attribution>
bool write_gc_memory_boundary(std::filesystem::path path, std::string_view phase,
    const std::string& campaign, const std::string& profile, const std::string& snapshot, Attribution&& attribution) {
    if (path.empty()) return true;
    path.replace_extension(std::string(".") + std::string(phase) + ".memory.json");
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    std::ofstream boundary(path, std::ios::binary | std::ios::trunc);
    if (boundary) {
        boundary << "{\"status\":\"partial_memory_boundary\",\"campaign\":" << std::quoted(campaign)
                 << ",\"profile\":" << std::quoted(profile)
                 << ",\"memory_census\":{\"units\":\"bytes\",\"scope\":\"listed_lower_bounds_and_overlapping_monitors_not_disjoint_process_ownership\",\"snapshots\":["
                 << snapshot << ",\"attribution\":";
        attribution(boundary);
        boundary << "}]}}\n";
    }
    boundary.close();
    return static_cast<bool>(boundary);
}
} // namespace eawr::diagnostics
