#include "viewer_host_internal.hpp"

namespace eawr::presentation::godot_backend {
namespace {









} // namespace

namespace viewer_host_detail {

[[nodiscard]] std::optional<vfs::Vfs> mount_content_layers(
    const std::filesystem::path& game_root,
    const std::filesystem::path& mod_root,
    const std::string_view profile,
    std::string& failure) {
    std::vector<std::pair<std::string, std::filesystem::path>> roots;
    if (profile != "" && profile != "eaw" && profile != "foc" && profile != "remake") {
        failure = "--eawr-profile must be eaw, foc or remake";
        return std::nullopt;
    }
    if (profile == "remake" && mod_root.empty()) {
        failure = "the Remake profile requires --eawr-mod-root";
        return std::nullopt;
    }
    if ((profile == "" && !mod_root.empty()) || profile == "remake") {
        for (const auto& layer : eawr::vfs::mod_chain_roots(mod_root)) roots.push_back(layer);
    }
    if (!game_root.empty()) {
        const std::filesystem::path expansion = game_root / "corruption" / "Data";
        const std::filesystem::path base = game_root / "GameData" / "Data";
        if (profile != "eaw" && std::filesystem::is_directory(expansion)) roots.emplace_back("expansion", expansion);
        if ((profile == "foc" || profile == "remake") && !std::filesystem::is_directory(expansion)) {
            failure = "the selected profile requires corruption/Data";
            return std::nullopt;
        }
        if (std::filesystem::is_directory(base)) roots.emplace_back("base", base);
    }
    if (roots.empty()) {
        failure = "atlas overlay requires --eawr-mod-root or a game root with Data layers";
        return std::nullopt;
    }
    std::vector<vfs::MountSpec> specs;
    auto chain = vfs::resolve_manifest_chain(roots);
    if (!chain) {
        failure = core::format_diagnostic(chain.error());
        return std::nullopt;
    }
    for (auto& manifest : chain.value()) specs.push_back(std::move(manifest.mount));
    auto filesystem = vfs::Vfs::mount(specs);
    if (!filesystem) {
        failure = core::format_diagnostic(filesystem.error());
        return std::nullopt;
    }
    return std::optional<vfs::Vfs>(std::move(filesystem.value()));
}

} // namespace viewer_host_detail

namespace {

} // namespace

















} // namespace eawr::presentation::godot_backend
